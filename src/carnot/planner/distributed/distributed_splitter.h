#pragma once
#include <memory>
#include <queue>
#include <string>
#include <unordered_set>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include "src/carnot/planner/compiler_state/compiler_state.h"
#include "src/carnot/planner/compiler_state/registry_info.h"
#include "src/carnot/planner/distributed/distributed_plan.h"
#include "src/carnot/planner/ir/ir_nodes.h"
#include "src/carnot/planner/ir/pattern_match.h"
#include "src/carnot/planner/rules/rule_executor.h"
#include "src/carnot/planner/rules/rules.h"

namespace pl {
namespace carnot {
namespace planner {
namespace distributed {

/**
 * @brief This rule inserts a GRPCBridge in front of blocking operators in a graph.
 * The rule finds MemorySources and then iterates through the
 * children until it hits a Blocking Operator or a Sink (which is also a blocking operator).
 *
 * Then the rule will insert a GRPCBridge (GRPCSink -> GRPCSourceGroup) between the parent_op
 * and the blocking op. The resulting IR should contain two subsets now:
 * 1. Where all sources are MemorySources and all sinks are GRPCSinks
 * 2. Where all sources are GRPCSourceGroups and all sinks are MemorySinks
 *
 * Graphically, we want to be able to convert the following logical plan:
 * MemSrc1
 *  |   \
 *  |    Agg
 *  |   /
 * Join
 *  |
 * Sink
 *
 * Into
 * MemSrc1
 *  |
 * GRPCSink(1)
 *
 * GRPCSource(1)
 *  |   \
 *  |    Agg
 *  |   /
 * Join
 *  |
 * Sink
 *
 * Where GRPCSink and GRPCSource are a bridge.
 *
 */
// TODO(philkuz) (PL-1470) Clean up splitter
class BlockingOperatorGRPCBridgeRule : public Rule {
 public:
  BlockingOperatorGRPCBridgeRule() : Rule(nullptr) {}

 private:
  StatusOr<bool> Apply(IRNode* ir_node) override;

  /**
   * @brief Recursive function that inserts a GRPCBridge between any
   * child or subsequent child of that op that is blocking. The recursion stops
   * for any child that is blocking.
   *
   * @param op: the operator to apply.,
   * @return StatusOr<bool>: true if a gRPC bridge is built, Errors are stored in Status.
   */
  StatusOr<bool> InsertGRPCBridgeForBlockingChildOperator(OperatorIR* op);

  /**
   * @brief Creates the GRPCBridge.
   *
   * @param parent_op: the parent operator that feeds into the new GRPCSink.
   * @param child_op: the child operator who's new parent should be the GRPCSourceGroup.
   * @return Status
   */
  Status AddNewGRPCNodes(OperatorIR* parent_op, OperatorIR* child_op);
  int64_t grpc_id_counter_ = 0;
};

/**
 * @brief A plan that is split around blocking nodes.
 * before_blocking: plan should have no blocking nodes and should end with nodes that feed into
 * GRPCSinks. No blocking nodes means there also should not be MemorySinks.
 *
 * after_blocking: plan should have no memory sources, feed data in from GRPCSources and sink data
 * into MemorySinks.
 *
 */
struct BlockingSplitPlan {
  // The plan that occcurs before blocking nodes.
  std::unique_ptr<IR> before_blocking;
  // The plan that occcurs after and including blocking nodes.
  std::unique_ptr<IR> after_blocking;
  // The that has both the before and after blocking nodes.
  std::unique_ptr<IR> original_plan;
};

/**
 * @brief Two sets of nodes that correspond to the nodes of the original plan for those
 * that occure before blocking nodes and those that occur after. Used as a return value for
 * DistributedSplitter::GetBlockingSplitGroupsFromIR.
 *
 */
struct BlockingSplitNodeIDGroups {
  absl::flat_hash_set<int64_t> before_blocking_nodes;
  absl::flat_hash_set<int64_t> after_blocking_nodes;
};

/**
 * @brief The DistributedSplitter splits apart the graph along Blocking Node lines. The result is
 * two new IR graphs -> one that is run on Carnot instances that pull up data from Stirling and the
 * other that is run on Carnot instances which accumulate data and run blocking operations.
 */
// TODO(philkuz) refactor DistributedSplitter to be an NVI and remove the static methods to clean up
// the headers.
class DistributedSplitter : public NotCopyable {
 public:
  /**
   * @brief The logical plan is split into two different pieces along blocking nodes lines.
   *
   */
  /**
   * @brief Inserts a GRPCBridge in front of blocking operators in a graph.
   * Inserts a GRPCBridge (GRPCSink -> GRPCSourceGroup) between the parent_op
   * and blocking ops. The returned SplitPlan should contain two IRs now:
   * 1. Where all sources are MemorySources and all sinks are GRPCSinks
   * 2. Where all sources are GRPCSourceGroups and all sinks are MemorySinks
   *
   * Graphically, we want to be able to convert the following logical plan:
   * MemSrc1
   *  |   \
   *  |    Agg
   *  |   /
   * Join
   *  |
   * Sink
   *
   * Into
   * MemSrc1
   *  |
   * GRPCSink(1)
   *
   * GRPCSource(1)
   *  |   \
   *  |    Agg
   *  |   /
   * Join
   *  |
   * Sink
   *
   * Where GRPCSink and GRPCSource are a bridge.
   *
   * @param logical_plan: the input logical_plan
   * @return StatusOr<std::unique_ptr<BlockingSplitPLan>>: the plan split along blocking lines.
   */
  static StatusOr<std::unique_ptr<BlockingSplitPlan>> SplitKelvinAndAgents(const IR* logical_plan);
  // TODO(philkuz) remove this old strategy and clean up all uses.
  static StatusOr<std::unique_ptr<BlockingSplitPlan>> SplitAtBlockingNode(const IR* logical_plan);

 private:
  static StatusOr<std::unique_ptr<IR>> ApplyGRPCBridgeRule(const IR* logical_plan);
  static BlockingSplitNodeIDGroups GetBlockingSplitGroupsFromIR(const IR* graph);

  /**
   * @brief Returns the list of operator ids from the graph that occur before the blocking node and
   * after the blocking node.
   *
   * Note: this does not include non Operator IDs. IR::Keep() with either set of ids
   * will not produce a working graph.
   *
   * @param logical_plan
   * @param on_kelvin
   * @return BlockingSplitNodeIDGroups
   */
  static BlockingSplitNodeIDGroups GetSplitGroups(
      const IR* logical_plan, const absl::flat_hash_map<int64_t, bool>& on_kelvin);

  static absl::flat_hash_map<int64_t, bool> GetKelvinNodes(const std::vector<OperatorIR*>& sources);
  static absl::flat_hash_map<OperatorIR*, std::vector<OperatorIR*>> GetEdgesToBreak(
      const IR* logical_plan, const absl::flat_hash_map<int64_t, bool>& on_kelvin,
      const std::vector<int64_t>& sources);

  static bool ExecutesOnDataStores(const udfspb::UDTFSourceExecutor& executor);
  static bool ExecutesOnRemoteProcessors(const udfspb::UDTFSourceExecutor& executor);
  static bool RunsOnDataStores(const std::vector<OperatorIR*> sources);
  static bool RunsOnRemoteProcessors(const std::vector<OperatorIR*> sources);
  static bool IsSourceOnKelvin(OperatorIR* source_op);
  static bool IsChildOpOnKelvin(bool is_parent_on_kelvin, OperatorIR* source_op);
  static StatusOr<std::unique_ptr<IR>> CreateGRPCBridge(
      const IR* logical_plan, const absl::flat_hash_map<int64_t, bool>& on_kelvin,
      const std::vector<int64_t>& sources);
  static StatusOr<GRPCSinkIR*> CreateGRPCSink(OperatorIR* parent_op, int64_t grpc_id);
  static StatusOr<GRPCSourceGroupIR*> CreateGRPCSourceGroup(OperatorIR* parent_op, int64_t grpc_id);
};
}  // namespace distributed
}  // namespace planner
}  // namespace carnot
}  // namespace pl
