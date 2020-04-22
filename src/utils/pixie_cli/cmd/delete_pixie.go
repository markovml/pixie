package cmd

import (
	"os"

	"github.com/spf13/cobra"
	"github.com/spf13/viper"
	"k8s.io/client-go/kubernetes"
	"pixielabs.ai/pixielabs/src/utils/pixie_cli/pkg/k8s"

	// Blank import necessary for kubeConfig to work.
	_ "k8s.io/client-go/plugin/pkg/client/auth/gcp"

	"pixielabs.ai/pixielabs/src/utils/pixie_cli/pkg/utils"
)

// DeleteCmd is the "delete" command.
var DeleteCmd = &cobra.Command{
	Use:   "delete",
	Short: "Deletes Pixie on the current K8s cluster",
	Run: func(cmd *cobra.Command, args []string) {
		clobberAll, _ := cmd.Flags().GetBool("clobber")
		ns, _ := cmd.Flags().GetString("namespace")
		deletePixie(ns, clobberAll)
	},
}

func init() {
	DeleteCmd.Flags().BoolP("clobber", "d", false, "Whether to delete all dependencies in the cluster")
	viper.BindPFlag("clobber", DeleteCmd.Flags().Lookup("clobber"))

	DeleteCmd.Flags().StringP("namespace", "n", "pl", "The namespace where pixie is located")
	viper.BindPFlag("namespace", DeleteCmd.Flags().Lookup("namespace"))
}

func deletePixie(ns string, clobberAll bool) {
	kubeConfig := k8s.GetConfig()
	clientset := k8s.GetClientset(kubeConfig)

	var deleteJob utils.Task

	if clobberAll {
		deleteJob = newTaskWrapper("Deleting namespace", func() error {
			return k8s.DeleteNamespace(clientset, ns)
		})
	} else {
		deleteJob = newTaskWrapper("Deleting Vizier pods/services", func() error {
			err := deleteVizier(clientset, ns)
			if err != nil {
				return err
			}
			return nil
		})
	}

	delJr := utils.NewSerialTaskRunner([]utils.Task{deleteJob})
	err := delJr.RunAndMonitor()
	if err != nil {
		os.Exit(1)
	}
}

func deleteVizier(clientset *kubernetes.Clientset, ns string) error {
	return k8s.DeleteAllResources(clientset, ns, "component=vizier")
}
