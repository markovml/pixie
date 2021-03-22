package main

import (
	"context"
	"crypto/tls"
	"net/http"
	"time"

	"github.com/cockroachdb/pebble"
	"github.com/coreos/etcd/clientv3"
	"github.com/coreos/etcd/pkg/transport"
	"github.com/nats-io/nats.go"
	log "github.com/sirupsen/logrus"
	"github.com/spf13/pflag"
	"github.com/spf13/viper"
	"google.golang.org/grpc"

	"pixielabs.ai/pixielabs/src/shared/services"
	"pixielabs.ai/pixielabs/src/shared/services/election"
	"pixielabs.ai/pixielabs/src/shared/services/healthz"
	"pixielabs.ai/pixielabs/src/shared/services/httpmiddleware"
	"pixielabs.ai/pixielabs/src/shared/services/server"
	version "pixielabs.ai/pixielabs/src/shared/version/go"
	"pixielabs.ai/pixielabs/src/vizier/services/metadata/controllers"
	"pixielabs.ai/pixielabs/src/vizier/services/metadata/controllers/agent"
	"pixielabs.ai/pixielabs/src/vizier/services/metadata/controllers/k8smeta"
	"pixielabs.ai/pixielabs/src/vizier/services/metadata/controllers/tracepoint"
	"pixielabs.ai/pixielabs/src/vizier/services/metadata/metadataenv"
	"pixielabs.ai/pixielabs/src/vizier/services/metadata/metadatapb"
	"pixielabs.ai/pixielabs/src/vizier/utils/datastore"
	"pixielabs.ai/pixielabs/src/vizier/utils/datastore/etcd"
	"pixielabs.ai/pixielabs/src/vizier/utils/datastore/pebbledb"
)

const (
	// pebbledbTTLDuration represents how often we evict from pebble.
	pebbledbTTLDuration = 5 * time.Minute
	// pebbleOpenDir is where the files live in the directory.
	pebbleOpenDir = "/metadata"
)

func init() {
	pflag.String("md_etcd_server", "https://etcd.pl.svc:2379", "The address to metadata etcd server.")
	pflag.String("cluster_id", "", "The Cluster ID to use for Pixie Cloud")
	pflag.Duration("max_expected_clock_skew", 2000, "Duration in ms of expected maximum clock skew in a cluster")
	pflag.Duration("renew_period", 5000, "Duration in ms of the time to wait to renew lease")
	pflag.String("pod_namespace", "pl", "The namespace this pod runs in. Used for leader elections")
	pflag.String("nats_url", "pl-nats", "The URL of NATS")
	pflag.Bool("use_etcd_operator", false, "Whether the etcd operator should be used instead of the persistent version.")
}

func mustInitEtcdDatastore() (*etcd.DataStore, func()) {
	var tlsConfig *tls.Config
	if !viper.GetBool("disable_ssl") {
		var err error
		tlsConfig, err = etcdTLSConfig()
		if err != nil {
			log.WithError(err).Fatal("Failed to load SSL for ETCD")
		}
	}

	// Connect to etcd.
	etcdClient, err := clientv3.New(clientv3.Config{
		Endpoints:   []string{viper.GetString("md_etcd_server")},
		DialTimeout: 5 * time.Second,
		TLS:         tlsConfig,
	})
	if err != nil {
		log.WithError(err).Fatal("Failed to connect to etcd at " + viper.GetString("md_etcd_server"))
	}

	etcdMgr := controllers.NewEtcdManager(etcdClient)
	etcdMgr.Run()
	cleanupFunc := func() {
		etcdMgr.Stop()
		etcdClient.Close()
	}
	return etcd.New(etcdClient), cleanupFunc
}

func mustInitPebbleDatastore() *pebbledb.DataStore {
	pebbleDb, err := pebble.Open(pebbleOpenDir, &pebble.Options{})
	if err != nil {
		log.WithError(err).Fatal("Failed to open pebble database.")
	}
	return pebbledb.New(pebbleDb, pebbledbTTLDuration)
}

func etcdTLSConfig() (*tls.Config, error) {
	tlsCert := viper.GetString("client_tls_cert")
	tlsKey := viper.GetString("client_tls_key")
	tlsCACert := viper.GetString("tls_ca_cert")

	tlsInfo := transport.TLSInfo{
		CertFile:      tlsCert,
		KeyFile:       tlsKey,
		TrustedCAFile: tlsCACert,
	}

	return tlsInfo.ClientConfig()
}

func main() {
	services.SetupService("metadata", 50400)
	services.SetupSSLClientFlags()
	services.PostFlagSetupAndParse()
	services.CheckServiceFlags()
	services.CheckSSLClientFlags()
	services.SetupServiceLogging()

	flush := services.InitDefaultSentry(viper.GetString("cluster_id"))
	defer flush()

	var nc *nats.Conn
	var err error
	if viper.GetBool("disable_ssl") {
		nc, err = nats.Connect(viper.GetString("nats_url"))
	} else {
		nc, err = nats.Connect(viper.GetString("nats_url"),
			nats.ClientCert(viper.GetString("client_tls_cert"), viper.GetString("client_tls_key")),
			nats.RootCAs(viper.GetString("tls_ca_cert")))
	}

	if err != nil {
		log.WithError(err).Fatal("Could not connect to NATS")
	}

	nc.SetErrorHandler(func(conn *nats.Conn, subscription *nats.Subscription, err error) {
		log.WithError(err).
			WithField("sub", subscription.Subject).
			Error("Got nats error")
	})

	// Set up leader election.
	isLeader := false
	leaderMgr, err := election.NewK8sLeaderElectionMgr(
		viper.GetString("pod_namespace"),
		viper.GetDuration("max_expected_clock_skew"),
		viper.GetDuration("renew_period"),
		"metadata-election",
	)
	if err != nil {
		log.WithError(err).Fatal("Failed to connect to leader election manager.")
	}

	// Cancel callback causes leader to resign.
	leaderCtx, cancel := context.WithCancel(context.Background())
	go func() {
		// Campaign in background. Metadata replicas that are not the leader should
		// do everything that the leader does, except write to the metadata store
		err = leaderMgr.Campaign(leaderCtx)
		if err != nil {
			log.WithError(err).Fatal("Failed to become leader")
		}
		log.Info("Gained leadership")
		isLeader = true
	}()

	// Resign leadership after the server stops.
	defer func() {
		log.Info("Resigning leadership")
		cancel()
	}()

	var dataStore datastore.MultiGetterSetterDeleterCloser
	var cleanupFunc func()
	if viper.GetBool("use_etcd_operator") {
		dataStore, cleanupFunc = mustInitEtcdDatastore()
		defer cleanupFunc()
	} else {
		dataStore = mustInitPebbleDatastore()
	}
	defer dataStore.Close()

	k8sMds := k8smeta.NewDatastore(dataStore)
	// Listen for K8s metadata updates.
	updateCh := make(chan *k8smeta.K8sResourceMessage)
	mdh := k8smeta.NewHandler(updateCh, k8sMds, nc)

	k8sMc, err := k8smeta.NewController(k8sMds, updateCh)
	defer k8sMc.Stop()

	ads := agent.NewDatastore(dataStore, 24*time.Hour)
	agtMgr := agent.NewManager(ads, mdh, nc)

	schemaQuitCh := make(chan struct{})
	defer close(schemaQuitCh)
	go func() {
		schemaTimer := time.NewTicker(1 * time.Minute)
		defer schemaTimer.Stop()
		for {
			select {
			case <-schemaQuitCh:
				return
			case <-schemaTimer.C:
				schemaErr := ads.PruneComputedSchema()
				if schemaErr != nil {
					log.WithError(schemaErr).Info("Failed to prune computed schema")
				}
			}
		}
	}()

	tds := tracepoint.NewDatastore(dataStore)
	// Initialize tracepoint handler.
	tracepointMgr := tracepoint.NewManager(tds, agtMgr, 30*time.Second)
	defer tracepointMgr.Close()

	mc, err := controllers.NewMessageBusController(nc, agtMgr, tracepointMgr,
		mdh, &isLeader)

	if err != nil {
		log.WithError(err).Fatal("Failed to connect to message bus")
	}
	defer mc.Close()

	// Set up server.
	env, err := metadataenv.New()
	if err != nil {
		log.WithError(err).Fatal("Failed to create api environment")
	}
	mux := http.NewServeMux()
	healthz.RegisterDefaultChecks(mux)

	svr := controllers.NewServer(env, agtMgr, tracepointMgr)
	log.Info("Metadata Server: " + version.GetVersion().ToString())

	// We bump up the max message size because agent metadata may be larger than 4MB. This is a
	// temporary change. In the future, we would like to page the agent metadata.
	maxMsgSize := grpc.MaxSendMsgSize(8 * 1024 * 1024)

	s := server.NewPLServer(env,
		httpmiddleware.WithBearerAuthMiddleware(env, mux), maxMsgSize)
	metadatapb.RegisterMetadataServiceServer(s.GRPCServer(), svr)
	metadatapb.RegisterMetadataTracepointServiceServer(s.GRPCServer(), svr)
	metadatapb.RegisterMetadataConfigServiceServer(s.GRPCServer(), svr)

	s.Start()
	s.StopOnInterrupt()
}
