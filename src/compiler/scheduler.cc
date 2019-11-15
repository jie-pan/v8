// Copyright 2013 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/scheduler.h"

#include <iomanip>

#include "src/base/iterator.h"
#include "src/codegen/tick-counter.h"
#include "src/compiler/common-operator.h"
#include "src/compiler/control-equivalence.h"
#include "src/compiler/graph.h"
#include "src/compiler/node-marker.h"
#include "src/compiler/node-properties.h"
#include "src/compiler/node.h"
#include "src/utils/bit-vector.h"
#include "src/zone/zone-containers.h"
#include "src/compiler/loop-variable-optimizer.h"
#include "src/compiler/loop-analysis.h"

namespace v8 {
namespace internal {
namespace compiler {

#define TRACE(...)                                       \
  do {                                                   \
    if (FLAG_trace_turbo_scheduler) PrintF(__VA_ARGS__); \
  } while (false)

Scheduler::Scheduler(Zone* zone, Graph* graph, MachineGraph* mcgraph,
                     Schedule* schedule, Flags flags,
                     size_t node_count_hint, TickCounter* tick_counter)
    : zone_(zone),
      graph_(graph),
      mcgraph_(mcgraph),
      schedule_(schedule),
      flags_(flags),
      scheduled_nodes_(zone),
      schedule_root_nodes_(zone),
      schedule_queue_(zone),
      node_data_(zone),
      tick_counter_(tick_counter) {
  node_data_.reserve(node_count_hint);
  node_data_.resize(graph->NodeCount(), DefaultSchedulerData());
}

Schedule* Scheduler::ComputeSchedule(Zone* zone, Graph* graph, Flags flags,
                                     TickCounter* tick_counter,
			                         MachineGraph* mcgraph,
                                     char* function_name) {
  Zone* schedule_zone =
      (flags & Scheduler::kTempSchedule) ? zone : graph->zone();

  // Reserve 10% more space for nodes if node splitting is enabled to try to
  // avoid resizing the vector since that would triple its zone memory usage.
  float node_hint_multiplier = (flags & Scheduler::kSplitNodes) ? 1.1 : 1;
  size_t node_count_hint = node_hint_multiplier * graph->NodeCount();

  Schedule* schedule =
      new (schedule_zone) Schedule(schedule_zone, node_count_hint);
  Scheduler scheduler(zone, graph, mcgraph, schedule, flags, node_count_hint,
                      tick_counter);

  if (FLAG_wasm_revec && graph->HasSimd()) {
    // TODO: check hardware avx support
    if (function_name != nullptr) {
      TRACE("revec--- function %s ", function_name);
      //PrintF("revec--- function %s \n", function_name);
    }
    scheduler.SelectLoopAndUpdateGraph();
  }

  scheduler.BuildCFG();
  scheduler.ComputeSpecialRPONumbering();
  scheduler.GenerateDominatorTree();

  scheduler.PrepareUses();
  scheduler.ScheduleEarly();
  scheduler.ScheduleLate();

  scheduler.SealFinalSchedule();

  if (FLAG_wasm_revec && graph->HasSimd()) {
    scheduler.MarkBlockInLoops();
  }

  return schedule;
}

Scheduler::SchedulerData Scheduler::DefaultSchedulerData() {
  SchedulerData def = {schedule_->start(), 0, kUnknown};
  return def;
}


Scheduler::SchedulerData* Scheduler::GetData(Node* node) {
  return &node_data_[node->id()];
}

Scheduler::Placement Scheduler::InitializePlacement(Node* node) {
  SchedulerData* data = GetData(node);
  if (data->placement_ == kFixed) {
    // Nothing to do for control nodes that have been already fixed in
    // the schedule.
    return data->placement_;
  }
  DCHECK_EQ(kUnknown, data->placement_);
  switch (node->opcode()) {
    case IrOpcode::kParameter:
    case IrOpcode::kOsrValue:
      // Parameters and OSR values are always fixed to the start block.
      data->placement_ = kFixed;
      break;
    case IrOpcode::kPhi:
    case IrOpcode::kEffectPhi: {
      // Phis and effect phis are fixed if their control inputs are, whereas
      // otherwise they are coupled to a floating control node.
      Placement p = GetPlacement(NodeProperties::GetControlInput(node));
      data->placement_ = (p == kFixed ? kFixed : kCoupled);
      break;
    }
#define DEFINE_CONTROL_CASE(V) case IrOpcode::k##V:
      CONTROL_OP_LIST(DEFINE_CONTROL_CASE)
#undef DEFINE_CONTROL_CASE
      {
        // Control nodes that were not control-reachable from end may float.
        data->placement_ = kSchedulable;
        break;
    }
    default:
      data->placement_ = kSchedulable;
      break;
  }
  return data->placement_;
}

Scheduler::Placement Scheduler::GetPlacement(Node* node) {
  return GetData(node)->placement_;
}

bool Scheduler::IsLive(Node* node) { return GetPlacement(node) != kUnknown; }

void Scheduler::UpdatePlacement(Node* node, Placement placement) {
  SchedulerData* data = GetData(node);
  if (data->placement_ == kUnknown) {
    // We only update control nodes from {kUnknown} to {kFixed}.  Ideally, we
    // should check that {node} is a control node (including exceptional calls),
    // but that is expensive.
    DCHECK_EQ(Scheduler::kFixed, placement);
    data->placement_ = placement;
    return;
  }

  switch (node->opcode()) {
    case IrOpcode::kParameter:
      // Parameters are fixed once and for all.
      UNREACHABLE();
    case IrOpcode::kPhi:
    case IrOpcode::kEffectPhi: {
      // Phis and effect phis are coupled to their respective blocks.
      DCHECK_EQ(Scheduler::kCoupled, data->placement_);
      DCHECK_EQ(Scheduler::kFixed, placement);
      Node* control = NodeProperties::GetControlInput(node);
      BasicBlock* block = schedule_->block(control);
      schedule_->AddNode(block, node);
      break;
    }
#define DEFINE_CONTROL_CASE(V) case IrOpcode::k##V:
      CONTROL_OP_LIST(DEFINE_CONTROL_CASE)
#undef DEFINE_CONTROL_CASE
      {
        // Control nodes force coupled uses to be placed.
        for (auto use : node->uses()) {
          if (GetPlacement(use) == Scheduler::kCoupled) {
            DCHECK_EQ(node, NodeProperties::GetControlInput(use));
            UpdatePlacement(use, placement);
          }
      }
      break;
    }
    default:
      DCHECK_EQ(Scheduler::kSchedulable, data->placement_);
      DCHECK_EQ(Scheduler::kScheduled, placement);
      break;
  }
  // Reduce the use count of the node's inputs to potentially make them
  // schedulable. If all the uses of a node have been scheduled, then the node
  // itself can be scheduled.
  for (Edge const edge : node->input_edges()) {
    DecrementUnscheduledUseCount(edge.to(), edge.index(), edge.from());
  }
  data->placement_ = placement;
}


bool Scheduler::IsCoupledControlEdge(Node* node, int index) {
  return GetPlacement(node) == kCoupled &&
         NodeProperties::FirstControlIndex(node) == index;
}


void Scheduler::IncrementUnscheduledUseCount(Node* node, int index,
                                             Node* from) {
  // Make sure that control edges from coupled nodes are not counted.
  if (IsCoupledControlEdge(from, index)) return;

  // Tracking use counts for fixed nodes is useless.
  if (GetPlacement(node) == kFixed) return;

  // Use count for coupled nodes is summed up on their control.
  if (GetPlacement(node) == kCoupled) {
    Node* control = NodeProperties::GetControlInput(node);
    return IncrementUnscheduledUseCount(control, index, from);
  }

  ++(GetData(node)->unscheduled_count_);
  if (FLAG_trace_turbo_scheduler) {
    TRACE("  Use count of #%d:%s (used by #%d:%s)++ = %d\n", node->id(),
          node->op()->mnemonic(), from->id(), from->op()->mnemonic(),
          GetData(node)->unscheduled_count_);
  }
}


void Scheduler::DecrementUnscheduledUseCount(Node* node, int index,
                                             Node* from) {
  // Make sure that control edges from coupled nodes are not counted.
  if (IsCoupledControlEdge(from, index)) return;

  // Tracking use counts for fixed nodes is useless.
  if (GetPlacement(node) == kFixed) return;

  // Use count for coupled nodes is summed up on their control.
  if (GetPlacement(node) == kCoupled) {
    Node* control = NodeProperties::GetControlInput(node);
    return DecrementUnscheduledUseCount(control, index, from);
  }

  DCHECK_LT(0, GetData(node)->unscheduled_count_);
  --(GetData(node)->unscheduled_count_);
  if (FLAG_trace_turbo_scheduler) {
    TRACE("  Use count of #%d:%s (used by #%d:%s)-- = %d\n", node->id(),
          node->op()->mnemonic(), from->id(), from->op()->mnemonic(),
          GetData(node)->unscheduled_count_);
  }
  if (GetData(node)->unscheduled_count_ == 0) {
    TRACE("    newly eligible #%d:%s\n", node->id(), node->op()->mnemonic());
    schedule_queue_.push(node);
  }
}


// -----------------------------------------------------------------------------
// Phase 1: Build control-flow graph.


// Internal class to build a control flow graph (i.e the basic blocks and edges
// between them within a Schedule) from the node graph. Visits control edges of
// the graph backwards from an end node in order to find the connected control
// subgraph, needed for scheduling.
class CFGBuilder : public ZoneObject {
 public:
  CFGBuilder(Zone* zone, Scheduler* scheduler)
      : zone_(zone),
        scheduler_(scheduler),
        schedule_(scheduler->schedule_),
        queued_(scheduler->graph_, 2),
        queue_(zone),
        control_(zone),
        component_entry_(nullptr),
        component_start_(nullptr),
        component_end_(nullptr) {}

  // Run the control flow graph construction algorithm by walking the graph
  // backwards from end through control edges, building and connecting the
  // basic blocks for control nodes.
  void Run() {
    ResetDataStructures();
    Queue(scheduler_->graph_->end());

    while (!queue_.empty()) {  // Breadth-first backwards traversal.
      scheduler_->tick_counter_->DoTick();
      Node* node = queue_.front();
      queue_.pop();
      int max = NodeProperties::PastControlIndex(node);
      for (int i = NodeProperties::FirstControlIndex(node); i < max; i++) {
        Queue(node->InputAt(i));
      }
    }

    for (NodeVector::iterator i = control_.begin(); i != control_.end(); ++i) {
      ConnectBlocks(*i);  // Connect block to its predecessor/successors.
    }
  }

  // Run the control flow graph construction for a minimal control-connected
  // component ending in {exit} and merge that component into an existing
  // control flow graph at the bottom of {block}.
  void Run(BasicBlock* block, Node* exit) {
    ResetDataStructures();
    Queue(exit);

    component_entry_ = nullptr;
    component_start_ = block;
    component_end_ = schedule_->block(exit);
    scheduler_->equivalence_->Run(exit);
    while (!queue_.empty()) {  // Breadth-first backwards traversal.
      scheduler_->tick_counter_->DoTick();
      Node* node = queue_.front();
      queue_.pop();

      // Use control dependence equivalence to find a canonical single-entry
      // single-exit region that makes up a minimal component to be scheduled.
      if (IsSingleEntrySingleExitRegion(node, exit)) {
        TRACE("Found SESE at #%d:%s\n", node->id(), node->op()->mnemonic());
        DCHECK(!component_entry_);
        component_entry_ = node;
        continue;
      }

      int max = NodeProperties::PastControlIndex(node);
      for (int i = NodeProperties::FirstControlIndex(node); i < max; i++) {
        Queue(node->InputAt(i));
      }
    }
    DCHECK(component_entry_);

    for (NodeVector::iterator i = control_.begin(); i != control_.end(); ++i) {
      ConnectBlocks(*i);  // Connect block to its predecessor/successors.
    }
  }

 private:
  friend class ScheduleLateNodeVisitor;
  friend class Scheduler;

  void FixNode(BasicBlock* block, Node* node) {
    schedule_->AddNode(block, node);
    scheduler_->UpdatePlacement(node, Scheduler::kFixed);
  }

  void Queue(Node* node) {
    // Mark the connected control nodes as they are queued.
    if (!queued_.Get(node)) {
      BuildBlocks(node);
      queue_.push(node);
      queued_.Set(node, true);
      control_.push_back(node);
    }
  }

  void BuildBlocks(Node* node) {
    switch (node->opcode()) {
      case IrOpcode::kEnd:
        FixNode(schedule_->end(), node);
        break;
      case IrOpcode::kStart:
        FixNode(schedule_->start(), node);
        break;
      case IrOpcode::kLoop:
      case IrOpcode::kMerge:
        BuildBlockForNode(node);
        break;
      case IrOpcode::kTerminate: {
        // Put Terminate in the loop to which it refers.
        Node* loop = NodeProperties::GetControlInput(node);
        BasicBlock* block = BuildBlockForNode(loop);
        FixNode(block, node);
        break;
      }
      case IrOpcode::kBranch:
      case IrOpcode::kSwitch:
        BuildBlocksForSuccessors(node);
        break;
#define BUILD_BLOCK_JS_CASE(Name) case IrOpcode::k##Name:
        JS_OP_LIST(BUILD_BLOCK_JS_CASE)
// JS opcodes are just like calls => fall through.
#undef BUILD_BLOCK_JS_CASE
      case IrOpcode::kCall:
        if (NodeProperties::IsExceptionalCall(node)) {
          BuildBlocksForSuccessors(node);
        }
        break;
      default:
        break;
    }
  }

  void ConnectBlocks(Node* node) {
    switch (node->opcode()) {
      case IrOpcode::kLoop:
      case IrOpcode::kMerge:
        ConnectMerge(node);
        break;
      case IrOpcode::kBranch:
        scheduler_->UpdatePlacement(node, Scheduler::kFixed);
        ConnectBranch(node);
        break;
      case IrOpcode::kSwitch:
        scheduler_->UpdatePlacement(node, Scheduler::kFixed);
        ConnectSwitch(node);
        break;
      case IrOpcode::kDeoptimize:
        scheduler_->UpdatePlacement(node, Scheduler::kFixed);
        ConnectDeoptimize(node);
        break;
      case IrOpcode::kTailCall:
        scheduler_->UpdatePlacement(node, Scheduler::kFixed);
        ConnectTailCall(node);
        break;
      case IrOpcode::kReturn:
        scheduler_->UpdatePlacement(node, Scheduler::kFixed);
        ConnectReturn(node);
        break;
      case IrOpcode::kThrow:
        scheduler_->UpdatePlacement(node, Scheduler::kFixed);
        ConnectThrow(node);
        break;
#define CONNECT_BLOCK_JS_CASE(Name) case IrOpcode::k##Name:
        JS_OP_LIST(CONNECT_BLOCK_JS_CASE)
// JS opcodes are just like calls => fall through.
#undef CONNECT_BLOCK_JS_CASE
      case IrOpcode::kCall:
        if (NodeProperties::IsExceptionalCall(node)) {
          scheduler_->UpdatePlacement(node, Scheduler::kFixed);
          ConnectCall(node);
        }
        break;
      default:
        break;
    }
  }

  BasicBlock* BuildBlockForNode(Node* node) {
    BasicBlock* block = schedule_->block(node);
    if (block == nullptr) {
      block = schedule_->NewBasicBlock();
      TRACE("Create block id:%d for #%d:%s\n", block->id().ToInt(), node->id(),
            node->op()->mnemonic());
      FixNode(block, node);
    }
    return block;
  }

  void BuildBlocksForSuccessors(Node* node) {
    size_t const successor_cnt = node->op()->ControlOutputCount();
    Node** successors = zone_->NewArray<Node*>(successor_cnt);
    NodeProperties::CollectControlProjections(node, successors, successor_cnt);
    for (size_t index = 0; index < successor_cnt; ++index) {
      BuildBlockForNode(successors[index]);
    }
  }

  void CollectSuccessorBlocks(Node* node, BasicBlock** successor_blocks,
                              size_t successor_cnt) {
    Node** successors = reinterpret_cast<Node**>(successor_blocks);
    NodeProperties::CollectControlProjections(node, successors, successor_cnt);
    for (size_t index = 0; index < successor_cnt; ++index) {
      successor_blocks[index] = schedule_->block(successors[index]);
    }
  }

  BasicBlock* FindPredecessorBlock(Node* node) {
    BasicBlock* predecessor_block = nullptr;
    while (true) {
      predecessor_block = schedule_->block(node);
      if (predecessor_block != nullptr) break;
      node = NodeProperties::GetControlInput(node);
    }
    return predecessor_block;
  }

  void ConnectCall(Node* call) {
    BasicBlock* successor_blocks[2];
    CollectSuccessorBlocks(call, successor_blocks, arraysize(successor_blocks));

    // Consider the exception continuation to be deferred.
    successor_blocks[1]->set_deferred(true);

    Node* call_control = NodeProperties::GetControlInput(call);
    BasicBlock* call_block = FindPredecessorBlock(call_control);
    TraceConnect(call, call_block, successor_blocks[0]);
    TraceConnect(call, call_block, successor_blocks[1]);
    schedule_->AddCall(call_block, call, successor_blocks[0],
                       successor_blocks[1]);
  }

  void ConnectBranch(Node* branch) {
    BasicBlock* successor_blocks[2];
    CollectSuccessorBlocks(branch, successor_blocks,
                           arraysize(successor_blocks));

    // Consider branch hints.
    switch (BranchHintOf(branch->op())) {
      case BranchHint::kNone:
        break;
      case BranchHint::kTrue:
        successor_blocks[1]->set_deferred(true);
        break;
      case BranchHint::kFalse:
        successor_blocks[0]->set_deferred(true);
        break;
    }

    if (branch == component_entry_) {
      TraceConnect(branch, component_start_, successor_blocks[0]);
      TraceConnect(branch, component_start_, successor_blocks[1]);
      schedule_->InsertBranch(component_start_, component_end_, branch,
                              successor_blocks[0], successor_blocks[1]);
    } else {
      Node* branch_control = NodeProperties::GetControlInput(branch);
      BasicBlock* branch_block = FindPredecessorBlock(branch_control);
      TraceConnect(branch, branch_block, successor_blocks[0]);
      TraceConnect(branch, branch_block, successor_blocks[1]);
      schedule_->AddBranch(branch_block, branch, successor_blocks[0],
                           successor_blocks[1]);
    }
  }

  void ConnectSwitch(Node* sw) {
    size_t const successor_count = sw->op()->ControlOutputCount();
    BasicBlock** successor_blocks =
        zone_->NewArray<BasicBlock*>(successor_count);
    CollectSuccessorBlocks(sw, successor_blocks, successor_count);

    if (sw == component_entry_) {
      for (size_t index = 0; index < successor_count; ++index) {
        TraceConnect(sw, component_start_, successor_blocks[index]);
      }
      schedule_->InsertSwitch(component_start_, component_end_, sw,
                              successor_blocks, successor_count);
    } else {
      Node* switch_control = NodeProperties::GetControlInput(sw);
      BasicBlock* switch_block = FindPredecessorBlock(switch_control);
      for (size_t index = 0; index < successor_count; ++index) {
        TraceConnect(sw, switch_block, successor_blocks[index]);
      }
      schedule_->AddSwitch(switch_block, sw, successor_blocks, successor_count);
    }
    for (size_t index = 0; index < successor_count; ++index) {
      if (BranchHintOf(successor_blocks[index]->front()->op()) ==
          BranchHint::kFalse) {
        successor_blocks[index]->set_deferred(true);
      }
    }
  }

  void ConnectMerge(Node* merge) {
    // Don't connect the special merge at the end to its predecessors.
    if (IsFinalMerge(merge)) return;

    BasicBlock* block = schedule_->block(merge);
    DCHECK_NOT_NULL(block);
    // For all of the merge's control inputs, add a goto at the end to the
    // merge's basic block.
    for (Node* const input : merge->inputs()) {
      BasicBlock* predecessor_block = FindPredecessorBlock(input);
      TraceConnect(merge, predecessor_block, block);
      schedule_->AddGoto(predecessor_block, block);
    }
  }

  void ConnectTailCall(Node* call) {
    Node* call_control = NodeProperties::GetControlInput(call);
    BasicBlock* call_block = FindPredecessorBlock(call_control);
    TraceConnect(call, call_block, nullptr);
    schedule_->AddTailCall(call_block, call);
  }

  void ConnectReturn(Node* ret) {
    Node* return_control = NodeProperties::GetControlInput(ret);
    BasicBlock* return_block = FindPredecessorBlock(return_control);
    TraceConnect(ret, return_block, nullptr);
    schedule_->AddReturn(return_block, ret);
  }

  void ConnectDeoptimize(Node* deopt) {
    Node* deoptimize_control = NodeProperties::GetControlInput(deopt);
    BasicBlock* deoptimize_block = FindPredecessorBlock(deoptimize_control);
    TraceConnect(deopt, deoptimize_block, nullptr);
    schedule_->AddDeoptimize(deoptimize_block, deopt);
  }

  void ConnectThrow(Node* thr) {
    Node* throw_control = NodeProperties::GetControlInput(thr);
    BasicBlock* throw_block = FindPredecessorBlock(throw_control);
    TraceConnect(thr, throw_block, nullptr);
    schedule_->AddThrow(throw_block, thr);
  }

  void TraceConnect(Node* node, BasicBlock* block, BasicBlock* succ) {
    DCHECK_NOT_NULL(block);
    if (succ == nullptr) {
      TRACE("Connect #%d:%s, id:%d -> end\n", node->id(),
            node->op()->mnemonic(), block->id().ToInt());
    } else {
      TRACE("Connect #%d:%s, id:%d -> id:%d\n", node->id(),
            node->op()->mnemonic(), block->id().ToInt(), succ->id().ToInt());
    }
  }

  bool IsFinalMerge(Node* node) {
    return (node->opcode() == IrOpcode::kMerge &&
            node == scheduler_->graph_->end()->InputAt(0));
  }

  bool IsSingleEntrySingleExitRegion(Node* entry, Node* exit) const {
    size_t entry_class = scheduler_->equivalence_->ClassOf(entry);
    size_t exit_class = scheduler_->equivalence_->ClassOf(exit);
    return entry != exit && entry_class == exit_class;
  }

  void ResetDataStructures() {
    control_.clear();
    DCHECK(queue_.empty());
    DCHECK(control_.empty());
  }

  Zone* zone_;
  Scheduler* scheduler_;
  Schedule* schedule_;
  NodeMarker<bool> queued_;      // Mark indicating whether node is queued.
  ZoneQueue<Node*> queue_;       // Queue used for breadth-first traversal.
  NodeVector control_;           // List of encountered control nodes.
  Node* component_entry_;        // Component single-entry node.
  BasicBlock* component_start_;  // Component single-entry block.
  BasicBlock* component_end_;    // Component single-exit block.
};


void Scheduler::BuildCFG() {
  TRACE("--- CREATING CFG -------------------------------------------\n");

  // Instantiate a new control equivalence algorithm for the graph.
  equivalence_ = new (zone_) ControlEquivalence(zone_, graph_);

  // Build a control-flow graph for the main control-connected component that
  // is being spanned by the graph's start and end nodes.
  control_flow_builder_ = new (zone_) CFGBuilder(zone_, this);
  control_flow_builder_->Run();

  // Initialize per-block data.
  // Reserve an extra 10% to avoid resizing vector when fusing floating control.
  scheduled_nodes_.reserve(schedule_->BasicBlockCount() * 1.1);
  scheduled_nodes_.resize(schedule_->BasicBlockCount());
}


// -----------------------------------------------------------------------------
// Phase 2: Compute special RPO and dominator tree.


// Compute the special reverse-post-order block ordering, which is essentially
// a RPO of the graph where loop bodies are contiguous. Properties:
// 1. If block A is a predecessor of B, then A appears before B in the order,
//    unless B is a loop header and A is in the loop headed at B
//    (i.e. A -> B is a backedge).
// => If block A dominates block B, then A appears before B in the order.
// => If block A is a loop header, A appears before all blocks in the loop
//    headed at A.
// 2. All loops are contiguous in the order (i.e. no intervening blocks that
//    do not belong to the loop.)
// Note a simple RPO traversal satisfies (1) but not (2).
class SpecialRPONumberer : public ZoneObject {
 public:
  SpecialRPONumberer(Zone* zone, Schedule* schedule)
      : zone_(zone),
        schedule_(schedule),
        order_(nullptr),
        beyond_end_(nullptr),
        loops_(zone),
        backedges_(zone),
        stack_(zone),
        previous_block_count_(0),
        empty_(0, zone) {}

  // Computes the special reverse-post-order for the main control flow graph,
  // that is for the graph spanned between the schedule's start and end blocks.
  void ComputeSpecialRPO() {
    DCHECK_EQ(0, schedule_->end()->SuccessorCount());
    DCHECK(!order_);  // Main order does not exist yet.
    ComputeAndInsertSpecialRPO(schedule_->start(), schedule_->end());
  }

  // Computes the special reverse-post-order for a partial control flow graph,
  // that is for the graph spanned between the given {entry} and {end} blocks,
  // then updates the existing ordering with this new information.
  void UpdateSpecialRPO(BasicBlock* entry, BasicBlock* end) {
    DCHECK(order_);  // Main order to be updated is present.
    ComputeAndInsertSpecialRPO(entry, end);
  }

  // Serialize the previously computed order as a special reverse-post-order
  // numbering for basic blocks into the final schedule.
  void SerializeRPOIntoSchedule() {
    int32_t number = 0;
    for (BasicBlock* b = order_; b != nullptr; b = b->rpo_next()) {
      b->set_rpo_number(number++);
      schedule_->rpo_order()->push_back(b);
    }
    BeyondEndSentinel()->set_rpo_number(number);
  }

  // Print and verify the special reverse-post-order.
  void PrintAndVerifySpecialRPO() {
#if DEBUG
    if (FLAG_trace_turbo_scheduler) PrintRPO();
    VerifySpecialRPO();
#endif
  }

  const ZoneVector<BasicBlock*>& GetOutgoingBlocks(BasicBlock* block) {
    if (HasLoopNumber(block)) {
      LoopInfo const& loop = loops_[GetLoopNumber(block)];
      if (loop.outgoing) return *loop.outgoing;
    }
    return empty_;
  }

 private:
  using Backedge = std::pair<BasicBlock*, size_t>;

  // Numbering for BasicBlock::rpo_number for this block traversal:
  static const int kBlockOnStack = -2;
  static const int kBlockVisited1 = -3;
  static const int kBlockVisited2 = -4;
  static const int kBlockUnvisited1 = -1;
  static const int kBlockUnvisited2 = kBlockVisited1;

  struct SpecialRPOStackFrame {
    BasicBlock* block;
    size_t index;
  };

  struct LoopInfo {
    BasicBlock* header;
    ZoneVector<BasicBlock*>* outgoing;
    BitVector* members;
    LoopInfo* prev;
    BasicBlock* end;
    BasicBlock* start;

    void AddOutgoing(Zone* zone, BasicBlock* block) {
      if (outgoing == nullptr) {
        outgoing = new (zone->New(sizeof(ZoneVector<BasicBlock*>)))
            ZoneVector<BasicBlock*>(zone);
      }
      outgoing->push_back(block);
    }
  };

  int Push(int depth, BasicBlock* child, int unvisited) {
    if (child->rpo_number() == unvisited) {
      stack_[depth].block = child;
      stack_[depth].index = 0;
      child->set_rpo_number(kBlockOnStack);
      return depth + 1;
    }
    return depth;
  }

  BasicBlock* PushFront(BasicBlock* head, BasicBlock* block) {
    block->set_rpo_next(head);
    return block;
  }

  static int GetLoopNumber(BasicBlock* block) { return block->loop_number(); }
  static void SetLoopNumber(BasicBlock* block, int loop_number) {
    return block->set_loop_number(loop_number);
  }
  static bool HasLoopNumber(BasicBlock* block) {
    return block->loop_number() >= 0;
  }

  // TODO(mstarzinger): We only need this special sentinel because some tests
  // use the schedule's end block in actual control flow (e.g. with end having
  // successors). Once this has been cleaned up we can use the end block here.
  BasicBlock* BeyondEndSentinel() {
    if (beyond_end_ == nullptr) {
      BasicBlock::Id id = BasicBlock::Id::FromInt(-1);
      beyond_end_ = new (schedule_->zone()) BasicBlock(schedule_->zone(), id);
    }
    return beyond_end_;
  }

  // Compute special RPO for the control flow graph between {entry} and {end},
  // mutating any existing order so that the result is still valid.
  void ComputeAndInsertSpecialRPO(BasicBlock* entry, BasicBlock* end) {
    // RPO should not have been serialized for this schedule yet.
    CHECK_EQ(kBlockUnvisited1, schedule_->start()->loop_number());
    CHECK_EQ(kBlockUnvisited1, schedule_->start()->rpo_number());
    CHECK_EQ(0, static_cast<int>(schedule_->rpo_order()->size()));

    // Find correct insertion point within existing order.
    BasicBlock* insertion_point = entry->rpo_next();
    BasicBlock* order = insertion_point;

    // Perform an iterative RPO traversal using an explicit stack,
    // recording backedges that form cycles. O(|B|).
    DCHECK_LT(previous_block_count_, schedule_->BasicBlockCount());
    stack_.resize(schedule_->BasicBlockCount() - previous_block_count_);
    previous_block_count_ = schedule_->BasicBlockCount();
    int stack_depth = Push(0, entry, kBlockUnvisited1);
    int num_loops = static_cast<int>(loops_.size());

    while (stack_depth > 0) {
      int current = stack_depth - 1;
      SpecialRPOStackFrame* frame = &stack_[current];

      if (frame->block != end &&
          frame->index < frame->block->SuccessorCount()) {
        // Process the next successor.
        BasicBlock* succ = frame->block->SuccessorAt(frame->index++);
        if (succ->rpo_number() == kBlockVisited1) continue;
        if (succ->rpo_number() == kBlockOnStack) {
          // The successor is on the stack, so this is a backedge (cycle).
          backedges_.push_back(Backedge(frame->block, frame->index - 1));
          if (!HasLoopNumber(succ)) {
            // Assign a new loop number to the header if it doesn't have one.
            SetLoopNumber(succ, num_loops++);
          }
        } else {
          // Push the successor onto the stack.
          DCHECK_EQ(kBlockUnvisited1, succ->rpo_number());
          stack_depth = Push(stack_depth, succ, kBlockUnvisited1);
        }
      } else {
        // Finished with all successors; pop the stack and add the block.
        order = PushFront(order, frame->block);
        frame->block->set_rpo_number(kBlockVisited1);
        stack_depth--;
      }
    }

    // If no loops were encountered, then the order we computed was correct.
    if (num_loops > static_cast<int>(loops_.size())) {
      // Otherwise, compute the loop information from the backedges in order
      // to perform a traversal that groups loop bodies together.
      ComputeLoopInfo(&stack_, num_loops, &backedges_);

      // Initialize the "loop stack". Note the entry could be a loop header.
      LoopInfo* loop =
          HasLoopNumber(entry) ? &loops_[GetLoopNumber(entry)] : nullptr;
      order = insertion_point;

      // Perform an iterative post-order traversal, visiting loop bodies before
      // edges that lead out of loops. Visits each block once, but linking loop
      // sections together is linear in the loop size, so overall is
      // O(|B| + max(loop_depth) * max(|loop|))
      stack_depth = Push(0, entry, kBlockUnvisited2);
      while (stack_depth > 0) {
        SpecialRPOStackFrame* frame = &stack_[stack_depth - 1];
        BasicBlock* block = frame->block;
        BasicBlock* succ = nullptr;

        if (block != end && frame->index < block->SuccessorCount()) {
          // Process the next normal successor.
          succ = block->SuccessorAt(frame->index++);
        } else if (HasLoopNumber(block)) {
          // Process additional outgoing edges from the loop header.
          if (block->rpo_number() == kBlockOnStack) {
            // Finish the loop body the first time the header is left on the
            // stack.
            DCHECK(loop != nullptr && loop->header == block);
            loop->start = PushFront(order, block);
            order = loop->end;
            block->set_rpo_number(kBlockVisited2);
            // Pop the loop stack and continue visiting outgoing edges within
            // the context of the outer loop, if any.
            loop = loop->prev;
            // We leave the loop header on the stack; the rest of this iteration
            // and later iterations will go through its outgoing edges list.
          }

          // Use the next outgoing edge if there are any.
          size_t outgoing_index = frame->index - block->SuccessorCount();
          LoopInfo* info = &loops_[GetLoopNumber(block)];
          DCHECK(loop != info);
          if (block != entry && info->outgoing != nullptr &&
              outgoing_index < info->outgoing->size()) {
            succ = info->outgoing->at(outgoing_index);
            frame->index++;
          }
        }

        if (succ != nullptr) {
          // Process the next successor.
          if (succ->rpo_number() == kBlockOnStack) continue;
          if (succ->rpo_number() == kBlockVisited2) continue;
          DCHECK_EQ(kBlockUnvisited2, succ->rpo_number());
          if (loop != nullptr && !loop->members->Contains(succ->id().ToInt())) {
            // The successor is not in the current loop or any nested loop.
            // Add it to the outgoing edges of this loop and visit it later.
            loop->AddOutgoing(zone_, succ);
          } else {
            // Push the successor onto the stack.
            stack_depth = Push(stack_depth, succ, kBlockUnvisited2);
            if (HasLoopNumber(succ)) {
              // Push the inner loop onto the loop stack.
              DCHECK(GetLoopNumber(succ) < num_loops);
              LoopInfo* next = &loops_[GetLoopNumber(succ)];
              next->end = order;
              next->prev = loop;
              loop = next;
            }
          }
        } else {
          // Finished with all successors of the current block.
          if (HasLoopNumber(block)) {
            // If we are going to pop a loop header, then add its entire body.
            LoopInfo* info = &loops_[GetLoopNumber(block)];
            for (BasicBlock* b = info->start; true; b = b->rpo_next()) {
              if (b->rpo_next() == info->end) {
                b->set_rpo_next(order);
                info->end = order;
                break;
              }
            }
            order = info->start;
          } else {
            // Pop a single node off the stack and add it to the order.
            order = PushFront(order, block);
            block->set_rpo_number(kBlockVisited2);
          }
          stack_depth--;
        }
      }
    }

    // Publish new order the first time.
    if (order_ == nullptr) order_ = order;

    // Compute the correct loop headers and set the correct loop ends.
    LoopInfo* current_loop = nullptr;
    BasicBlock* current_header = entry->loop_header();
    int32_t loop_depth = entry->loop_depth();
    if (entry->IsLoopHeader()) --loop_depth;  // Entry might be a loop header.
    for (BasicBlock* b = order; b != insertion_point; b = b->rpo_next()) {
      BasicBlock* current = b;

      // Reset BasicBlock::rpo_number again.
      current->set_rpo_number(kBlockUnvisited1);

      // Finish the previous loop(s) if we just exited them.
      while (current_header != nullptr &&
             current == current_header->loop_end()) {
        DCHECK(current_header->IsLoopHeader());
        DCHECK_NOT_NULL(current_loop);
        current_loop = current_loop->prev;
        current_header =
            current_loop == nullptr ? nullptr : current_loop->header;
        --loop_depth;
      }
      current->set_loop_header(current_header);

      // Push a new loop onto the stack if this loop is a loop header.
      if (HasLoopNumber(current)) {
        ++loop_depth;
        current_loop = &loops_[GetLoopNumber(current)];
        BasicBlock* end = current_loop->end;
        current->set_loop_end(end == nullptr ? BeyondEndSentinel() : end);
        current_header = current_loop->header;
        TRACE("id:%d is a loop header, increment loop depth to %d\n",
              current->id().ToInt(), loop_depth);
      }

      current->set_loop_depth(loop_depth);

      if (current->loop_header() == nullptr) {
        TRACE("id:%d is not in a loop (depth == %d)\n", current->id().ToInt(),
              current->loop_depth());
      } else {
        TRACE("id:%d has loop header id:%d, (depth == %d)\n",
              current->id().ToInt(), current->loop_header()->id().ToInt(),
              current->loop_depth());
      }
    }
  }

  // Computes loop membership from the backedges of the control flow graph.
  void ComputeLoopInfo(ZoneVector<SpecialRPOStackFrame>* queue,
                       size_t num_loops, ZoneVector<Backedge>* backedges) {
    // Extend existing loop membership vectors.
    for (LoopInfo& loop : loops_) {
      loop.members->Resize(static_cast<int>(schedule_->BasicBlockCount()),
                           zone_);
    }

    // Extend loop information vector.
    loops_.resize(num_loops, LoopInfo());

    // Compute loop membership starting from backedges.
    // O(max(loop_depth) * max(|loop|)
    for (size_t i = 0; i < backedges->size(); i++) {
      BasicBlock* member = backedges->at(i).first;
      BasicBlock* header = member->SuccessorAt(backedges->at(i).second);
      size_t loop_num = GetLoopNumber(header);
      if (loops_[loop_num].header == nullptr) {
        loops_[loop_num].header = header;
        loops_[loop_num].members = new (zone_)
            BitVector(static_cast<int>(schedule_->BasicBlockCount()), zone_);
      }

      int queue_length = 0;
      if (member != header) {
        // As long as the header doesn't have a backedge to itself,
        // Push the member onto the queue and process its predecessors.
        if (!loops_[loop_num].members->Contains(member->id().ToInt())) {
          loops_[loop_num].members->Add(member->id().ToInt());
        }
        (*queue)[queue_length++].block = member;
      }

      // Propagate loop membership backwards. All predecessors of M up to the
      // loop header H are members of the loop too. O(|blocks between M and H|).
      while (queue_length > 0) {
        BasicBlock* block = (*queue)[--queue_length].block;
        for (size_t i = 0; i < block->PredecessorCount(); i++) {
          BasicBlock* pred = block->PredecessorAt(i);
          if (pred != header) {
            if (!loops_[loop_num].members->Contains(pred->id().ToInt())) {
              loops_[loop_num].members->Add(pred->id().ToInt());
              (*queue)[queue_length++].block = pred;
            }
          }
        }
      }
    }
  }

#if DEBUG
  void PrintRPO() {
    StdoutStream os;
    os << "RPO with " << loops_.size() << " loops";
    if (loops_.size() > 0) {
      os << " (";
      for (size_t i = 0; i < loops_.size(); i++) {
        if (i > 0) os << " ";
        os << "id:" << loops_[i].header->id();
      }
      os << ")";
    }
    os << ":\n";

    for (BasicBlock* block = order_; block != nullptr;
         block = block->rpo_next()) {
      os << std::setw(5) << "B" << block->rpo_number() << ":";
      for (size_t i = 0; i < loops_.size(); i++) {
        bool range = loops_[i].header->LoopContains(block);
        bool membership = loops_[i].header != block && range;
        os << (membership ? " |" : "  ");
        os << (range ? "x" : " ");
      }
      os << "  id:" << block->id() << ": ";
      if (block->loop_end() != nullptr) {
        os << " range: [B" << block->rpo_number() << ", B"
           << block->loop_end()->rpo_number() << ")";
      }
      if (block->loop_header() != nullptr) {
        os << " header: id:" << block->loop_header()->id();
      }
      if (block->loop_depth() > 0) {
        os << " depth: " << block->loop_depth();
      }
      os << "\n";
    }
  }

  void VerifySpecialRPO() {
    BasicBlockVector* order = schedule_->rpo_order();
    DCHECK_LT(0, order->size());
    DCHECK_EQ(0, (*order)[0]->id().ToInt());  // entry should be first.

    for (size_t i = 0; i < loops_.size(); i++) {
      LoopInfo* loop = &loops_[i];
      BasicBlock* header = loop->header;
      BasicBlock* end = header->loop_end();

      DCHECK_NOT_NULL(header);
      DCHECK_LE(0, header->rpo_number());
      DCHECK_LT(header->rpo_number(), order->size());
      DCHECK_NOT_NULL(end);
      DCHECK_LE(end->rpo_number(), order->size());
      DCHECK_GT(end->rpo_number(), header->rpo_number());
      DCHECK_NE(header->loop_header(), header);

      // Verify the start ... end list relationship.
      int links = 0;
      BasicBlock* block = loop->start;
      DCHECK_EQ(header, block);
      bool end_found;
      while (true) {
        if (block == nullptr || block == loop->end) {
          end_found = (loop->end == block);
          break;
        }
        // The list should be in same order as the final result.
        DCHECK(block->rpo_number() == links + header->rpo_number());
        links++;
        block = block->rpo_next();
        DCHECK_LT(links, static_cast<int>(2 * order->size()));  // cycle?
      }
      DCHECK_LT(0, links);
      DCHECK_EQ(links, end->rpo_number() - header->rpo_number());
      DCHECK(end_found);

      // Check loop depth of the header.
      int loop_depth = 0;
      for (LoopInfo* outer = loop; outer != nullptr; outer = outer->prev) {
        loop_depth++;
      }
      DCHECK_EQ(loop_depth, header->loop_depth());

      // Check the contiguousness of loops.
      int count = 0;
      for (int j = 0; j < static_cast<int>(order->size()); j++) {
        BasicBlock* block = order->at(j);
        DCHECK_EQ(block->rpo_number(), j);
        if (j < header->rpo_number() || j >= end->rpo_number()) {
          DCHECK(!header->LoopContains(block));
        } else {
          DCHECK(header->LoopContains(block));
          DCHECK_GE(block->loop_depth(), loop_depth);
          count++;
        }
      }
      DCHECK_EQ(links, count);
    }
  }
#endif  // DEBUG

  Zone* zone_;
  Schedule* schedule_;
  BasicBlock* order_;
  BasicBlock* beyond_end_;
  ZoneVector<LoopInfo> loops_;
  ZoneVector<Backedge> backedges_;
  ZoneVector<SpecialRPOStackFrame> stack_;
  size_t previous_block_count_;
  ZoneVector<BasicBlock*> const empty_;
};


BasicBlockVector* Scheduler::ComputeSpecialRPO(Zone* zone, Schedule* schedule) {
  SpecialRPONumberer numberer(zone, schedule);
  numberer.ComputeSpecialRPO();
  numberer.SerializeRPOIntoSchedule();
  numberer.PrintAndVerifySpecialRPO();
  return schedule->rpo_order();
}


void Scheduler::ComputeSpecialRPONumbering() {
  TRACE("--- COMPUTING SPECIAL RPO ----------------------------------\n");

  // Compute the special reverse-post-order for basic blocks.
  special_rpo_ = new (zone_) SpecialRPONumberer(zone_, schedule_);
  special_rpo_->ComputeSpecialRPO();
}


void Scheduler::PropagateImmediateDominators(BasicBlock* block) {
  for (/*nop*/; block != nullptr; block = block->rpo_next()) {
    auto pred = block->predecessors().begin();
    auto end = block->predecessors().end();
    DCHECK(pred != end);  // All blocks except start have predecessors.
    BasicBlock* dominator = *pred;
    bool deferred = dominator->deferred();
    // For multiple predecessors, walk up the dominator tree until a common
    // dominator is found. Visitation order guarantees that all predecessors
    // except for backwards edges have been visited.
    for (++pred; pred != end; ++pred) {
      // Don't examine backwards edges.
      if ((*pred)->dominator_depth() < 0) continue;
      dominator = BasicBlock::GetCommonDominator(dominator, *pred);
      deferred = deferred & (*pred)->deferred();
    }
    block->set_dominator(dominator);
    block->set_dominator_depth(dominator->dominator_depth() + 1);
    block->set_deferred(deferred | block->deferred());
    TRACE("Block id:%d's idom is id:%d, depth = %d\n", block->id().ToInt(),
          dominator->id().ToInt(), block->dominator_depth());
  }
}

void Scheduler::GenerateDominatorTree(Schedule* schedule) {
  // Seed start block to be the first dominator.
  schedule->start()->set_dominator_depth(0);

  // Build the block dominator tree resulting from the above seed.
  PropagateImmediateDominators(schedule->start()->rpo_next());
}

void Scheduler::GenerateDominatorTree() {
  TRACE("--- IMMEDIATE BLOCK DOMINATORS -----------------------------\n");
  GenerateDominatorTree(schedule_);
}

// -----------------------------------------------------------------------------
// Phase 3: Prepare use counts for nodes.


class PrepareUsesVisitor {
 public:
  explicit PrepareUsesVisitor(Scheduler* scheduler)
      : scheduler_(scheduler), schedule_(scheduler->schedule_) {}

  void Pre(Node* node) {
    if (scheduler_->InitializePlacement(node) == Scheduler::kFixed) {
      // Fixed nodes are always roots for schedule late.
      scheduler_->schedule_root_nodes_.push_back(node);
      if (!schedule_->IsScheduled(node)) {
        // Make sure root nodes are scheduled in their respective blocks.
        TRACE("Scheduling fixed position node #%d:%s\n", node->id(),
              node->op()->mnemonic());
        IrOpcode::Value opcode = node->opcode();
        BasicBlock* block =
            opcode == IrOpcode::kParameter
                ? schedule_->start()
                : schedule_->block(NodeProperties::GetControlInput(node));
        DCHECK_NOT_NULL(block);
        schedule_->AddNode(block, node);
      }
    }
  }

  void PostEdge(Node* from, int index, Node* to) {
    // If the edge is from an unscheduled node, then tally it in the use count
    // for all of its inputs. The same criterion will be used in ScheduleLate
    // for decrementing use counts.
    if (!schedule_->IsScheduled(from)) {
      DCHECK_NE(Scheduler::kFixed, scheduler_->GetPlacement(from));
      scheduler_->IncrementUnscheduledUseCount(to, index, from);
    }
  }

 private:
  Scheduler* scheduler_;
  Schedule* schedule_;
};


void Scheduler::PrepareUses() {
  TRACE("--- PREPARE USES -------------------------------------------\n");

  // Count the uses of every node, which is used to ensure that all of a
  // node's uses are scheduled before the node itself.
  PrepareUsesVisitor prepare_uses(this);

  // TODO(turbofan): simplify the careful pre/post ordering here.
  BoolVector visited(graph_->NodeCount(), false, zone_);
  ZoneStack<Node::InputEdges::iterator> stack(zone_);
  Node* node = graph_->end();
  prepare_uses.Pre(node);
  visited[node->id()] = true;
  stack.push(node->input_edges().begin());
  while (!stack.empty()) {
    tick_counter_->DoTick();
    Edge edge = *stack.top();
    Node* node = edge.to();
    if (visited[node->id()]) {
      prepare_uses.PostEdge(edge.from(), edge.index(), edge.to());
      if (++stack.top() == edge.from()->input_edges().end()) stack.pop();
    } else {
      prepare_uses.Pre(node);
      visited[node->id()] = true;
      if (node->InputCount() > 0) stack.push(node->input_edges().begin());
    }
  }
}


// -----------------------------------------------------------------------------
// Phase 4: Schedule nodes early.


class ScheduleEarlyNodeVisitor {
 public:
  ScheduleEarlyNodeVisitor(Zone* zone, Scheduler* scheduler)
      : scheduler_(scheduler), schedule_(scheduler->schedule_), queue_(zone) {}

  // Run the schedule early algorithm on a set of fixed root nodes.
  void Run(NodeVector* roots) {
    for (Node* const root : *roots) {
      queue_.push(root);
      while (!queue_.empty()) {
        scheduler_->tick_counter_->DoTick();
        VisitNode(queue_.front());
        queue_.pop();
      }
    }
  }

 private:
  // Visits one node from the queue and propagates its current schedule early
  // position to all uses. This in turn might push more nodes onto the queue.
  void VisitNode(Node* node) {
    Scheduler::SchedulerData* data = scheduler_->GetData(node);

    // Fixed nodes already know their schedule early position.
    if (scheduler_->GetPlacement(node) == Scheduler::kFixed) {
      data->minimum_block_ = schedule_->block(node);
      TRACE("Fixing #%d:%s minimum_block = id:%d, dominator_depth = %d\n",
            node->id(), node->op()->mnemonic(),
            data->minimum_block_->id().ToInt(),
            data->minimum_block_->dominator_depth());
    }

    // No need to propagate unconstrained schedule early positions.
    if (data->minimum_block_ == schedule_->start()) return;

    // Propagate schedule early position.
    DCHECK_NOT_NULL(data->minimum_block_);
    for (auto use : node->uses()) {
      if (scheduler_->IsLive(use)) {
        PropagateMinimumPositionToNode(data->minimum_block_, use);
      }
    }
  }

  // Propagates {block} as another minimum position into the given {node}. This
  // has the net effect of computing the minimum dominator block of {node} that
  // still post-dominates all inputs to {node} when the queue is processed.
  void PropagateMinimumPositionToNode(BasicBlock* block, Node* node) {
    Scheduler::SchedulerData* data = scheduler_->GetData(node);

    // No need to propagate to fixed node, it's guaranteed to be a root.
    if (scheduler_->GetPlacement(node) == Scheduler::kFixed) return;

    // Coupled nodes influence schedule early position of their control.
    if (scheduler_->GetPlacement(node) == Scheduler::kCoupled) {
      Node* control = NodeProperties::GetControlInput(node);
      PropagateMinimumPositionToNode(block, control);
    }

    // Propagate new position if it is deeper down the dominator tree than the
    // current. Note that all inputs need to have minimum block position inside
    // the dominator chain of {node}'s minimum block position.
    DCHECK(InsideSameDominatorChain(block, data->minimum_block_));
    if (block->dominator_depth() > data->minimum_block_->dominator_depth()) {
      data->minimum_block_ = block;
      queue_.push(node);
      TRACE("Propagating #%d:%s minimum_block = id:%d, dominator_depth = %d\n",
            node->id(), node->op()->mnemonic(),
            data->minimum_block_->id().ToInt(),
            data->minimum_block_->dominator_depth());
    }
  }

#if DEBUG
  bool InsideSameDominatorChain(BasicBlock* b1, BasicBlock* b2) {
    BasicBlock* dominator = BasicBlock::GetCommonDominator(b1, b2);
    return dominator == b1 || dominator == b2;
  }
#endif

  Scheduler* scheduler_;
  Schedule* schedule_;
  ZoneQueue<Node*> queue_;
};


void Scheduler::ScheduleEarly() {
  TRACE("--- SCHEDULE EARLY -----------------------------------------\n");
  if (FLAG_trace_turbo_scheduler) {
    TRACE("roots: ");
    for (Node* node : schedule_root_nodes_) {
      TRACE("#%d:%s ", node->id(), node->op()->mnemonic());
    }
    TRACE("\n");
  }

  // Compute the minimum block for each node thereby determining the earliest
  // position each node could be placed within a valid schedule.
  ScheduleEarlyNodeVisitor schedule_early_visitor(zone_, this);
  schedule_early_visitor.Run(&schedule_root_nodes_);
}


// -----------------------------------------------------------------------------
// Phase 5: Schedule nodes late.


class ScheduleLateNodeVisitor {
 public:
  ScheduleLateNodeVisitor(Zone* zone, Scheduler* scheduler)
      : zone_(zone),
        scheduler_(scheduler),
        schedule_(scheduler_->schedule_),
        marked_(scheduler->zone_),
        marking_queue_(scheduler->zone_) {}

  // Run the schedule late algorithm on a set of fixed root nodes.
  void Run(NodeVector* roots) {
    for (Node* const root : *roots) {
      ProcessQueue(root);
    }
  }

 private:
  void ProcessQueue(Node* root) {
    ZoneQueue<Node*>* queue = &(scheduler_->schedule_queue_);
    for (Node* node : root->inputs()) {
      // Don't schedule coupled nodes on their own.
      if (scheduler_->GetPlacement(node) == Scheduler::kCoupled) {
        node = NodeProperties::GetControlInput(node);
      }

      // Test schedulability condition by looking at unscheduled use count.
      if (scheduler_->GetData(node)->unscheduled_count_ != 0) continue;

      queue->push(node);
      do {
        scheduler_->tick_counter_->DoTick();
        Node* const node = queue->front();
        queue->pop();
        VisitNode(node);
      } while (!queue->empty());
    }
  }

  // Visits one node from the queue of schedulable nodes and determines its
  // schedule late position. Also hoists nodes out of loops to find a more
  // optimal scheduling position.
  void VisitNode(Node* node) {
    DCHECK_EQ(0, scheduler_->GetData(node)->unscheduled_count_);

    // Don't schedule nodes that are already scheduled.
    if (schedule_->IsScheduled(node)) return;
    DCHECK_EQ(Scheduler::kSchedulable, scheduler_->GetPlacement(node));

    // Determine the dominating block for all of the uses of this node. It is
    // the latest block that this node can be scheduled in.
    TRACE("Scheduling #%d:%s\n", node->id(), node->op()->mnemonic());
    BasicBlock* block = GetCommonDominatorOfUses(node);
    DCHECK_NOT_NULL(block);

    // The schedule early block dominates the schedule late block.
    BasicBlock* min_block = scheduler_->GetData(node)->minimum_block_;
    DCHECK_EQ(min_block, BasicBlock::GetCommonDominator(block, min_block));
    TRACE(
        "Schedule late of #%d:%s is id:%d at loop depth %d, minimum = id:%d\n",
        node->id(), node->op()->mnemonic(), block->id().ToInt(),
        block->loop_depth(), min_block->id().ToInt());

    // Hoist nodes out of loops if possible. Nodes can be hoisted iteratively
    // into enclosing loop pre-headers until they would precede their schedule
    // early position.
    BasicBlock* hoist_block = GetHoistBlock(block);
    if (hoist_block &&
        hoist_block->dominator_depth() >= min_block->dominator_depth()) {
      do {
        TRACE("  hoisting #%d:%s to block id:%d\n", node->id(),
              node->op()->mnemonic(), hoist_block->id().ToInt());
        DCHECK_LT(hoist_block->loop_depth(), block->loop_depth());
        block = hoist_block;
        hoist_block = GetHoistBlock(hoist_block);
      } while (hoist_block &&
               hoist_block->dominator_depth() >= min_block->dominator_depth());
    } else if (scheduler_->flags_ & Scheduler::kSplitNodes) {
      // Split the {node} if beneficial and return the new {block} for it.
      block = SplitNode(block, node);
    }

    // Schedule the node or a floating control structure.
    if (IrOpcode::IsMergeOpcode(node->opcode())) {
      ScheduleFloatingControl(block, node);
    } else if (node->opcode() == IrOpcode::kFinishRegion) {
      ScheduleRegion(block, node);
    } else {
      ScheduleNode(block, node);
    }
  }

  bool IsMarked(BasicBlock* block) const {
    DCHECK_LT(block->id().ToSize(), marked_.size());
    return marked_[block->id().ToSize()];
  }

  void Mark(BasicBlock* block) { marked_[block->id().ToSize()] = true; }

  // Mark {block} and push its non-marked predecessor on the marking queue.
  void MarkBlock(BasicBlock* block) {
    DCHECK_LT(block->id().ToSize(), marked_.size());
    Mark(block);
    for (BasicBlock* pred_block : block->predecessors()) {
      if (IsMarked(pred_block)) continue;
      marking_queue_.push_back(pred_block);
    }
  }

  BasicBlock* SplitNode(BasicBlock* block, Node* node) {
    // For now, we limit splitting to pure nodes.
    if (!node->op()->HasProperty(Operator::kPure)) return block;
    // TODO(titzer): fix the special case of splitting of projections.
    if (node->opcode() == IrOpcode::kProjection) return block;

    // The {block} is common dominator of all uses of {node}, so we cannot
    // split anything unless the {block} has at least two successors.
    DCHECK_EQ(block, GetCommonDominatorOfUses(node));
    if (block->SuccessorCount() < 2) return block;

    // Clear marking bits.
    DCHECK(marking_queue_.empty());
    std::fill(marked_.begin(), marked_.end(), false);
    marked_.resize(schedule_->BasicBlockCount() + 1, false);

    // Check if the {node} has uses in {block}.
    for (Edge edge : node->use_edges()) {
      if (!scheduler_->IsLive(edge.from())) continue;
      BasicBlock* use_block = GetBlockForUse(edge);
      if (use_block == nullptr || IsMarked(use_block)) continue;
      if (use_block == block) {
        TRACE("  not splitting #%d:%s, it is used in id:%d\n", node->id(),
              node->op()->mnemonic(), block->id().ToInt());
        marking_queue_.clear();
        return block;
      }
      MarkBlock(use_block);
    }

    // Compute transitive marking closure; a block is marked if all its
    // successors are marked.
    do {
      BasicBlock* top_block = marking_queue_.front();
      marking_queue_.pop_front();
      if (IsMarked(top_block)) continue;
      bool marked = true;
      for (BasicBlock* successor : top_block->successors()) {
        if (!IsMarked(successor)) {
          marked = false;
          break;
        }
      }
      if (marked) MarkBlock(top_block);
    } while (!marking_queue_.empty());

    // If the (common dominator) {block} is marked, we know that all paths from
    // {block} to the end contain at least one use of {node}, and hence there's
    // no point in splitting the {node} in this case.
    if (IsMarked(block)) {
      TRACE("  not splitting #%d:%s, its common dominator id:%d is perfect\n",
            node->id(), node->op()->mnemonic(), block->id().ToInt());
      return block;
    }

    // Split {node} for uses according to the previously computed marking
    // closure. Every marking partition has a unique dominator, which get's a
    // copy of the {node} with the exception of the first partition, which get's
    // the {node} itself.
    ZoneMap<BasicBlock*, Node*> dominators(scheduler_->zone_);
    for (Edge edge : node->use_edges()) {
      if (!scheduler_->IsLive(edge.from())) continue;
      BasicBlock* use_block = GetBlockForUse(edge);
      if (use_block == nullptr) continue;
      while (IsMarked(use_block->dominator())) {
        use_block = use_block->dominator();
      }
      auto& use_node = dominators[use_block];
      if (use_node == nullptr) {
        if (dominators.size() == 1u) {
          // Place the {node} at {use_block}.
          block = use_block;
          use_node = node;
          TRACE("  pushing #%d:%s down to id:%d\n", node->id(),
                node->op()->mnemonic(), block->id().ToInt());
        } else {
          // Place a copy of {node} at {use_block}.
          use_node = CloneNode(node);
          TRACE("  cloning #%d:%s for id:%d\n", use_node->id(),
                use_node->op()->mnemonic(), use_block->id().ToInt());
          scheduler_->schedule_queue_.push(use_node);
        }
      }
      edge.UpdateTo(use_node);
    }
    return block;
  }

  BasicBlock* GetHoistBlock(BasicBlock* block) {
    if (block->IsLoopHeader()) return block->dominator();
    // We have to check to make sure that the {block} dominates all
    // of the outgoing blocks.  If it doesn't, then there is a path
    // out of the loop which does not execute this {block}, so we
    // can't hoist operations from this {block} out of the loop, as
    // that would introduce additional computations.
    if (BasicBlock* header_block = block->loop_header()) {
      for (BasicBlock* outgoing_block :
           scheduler_->special_rpo_->GetOutgoingBlocks(header_block)) {
        if (BasicBlock::GetCommonDominator(block, outgoing_block) != block) {
          return nullptr;
        }
      }
      return header_block->dominator();
    }
    return nullptr;
  }

  BasicBlock* GetCommonDominatorOfUses(Node* node) {
    BasicBlock* block = nullptr;
    for (Edge edge : node->use_edges()) {
      if (!scheduler_->IsLive(edge.from())) continue;
      BasicBlock* use_block = GetBlockForUse(edge);
      block = block == nullptr
                  ? use_block
                  : use_block == nullptr
                        ? block
                        : BasicBlock::GetCommonDominator(block, use_block);
    }
    return block;
  }

  BasicBlock* FindPredecessorBlock(Node* node) {
    return scheduler_->control_flow_builder_->FindPredecessorBlock(node);
  }

  BasicBlock* GetBlockForUse(Edge edge) {
    // TODO(titzer): ignore uses from dead nodes (not visited in PrepareUses()).
    // Dead uses only occur if the graph is not trimmed before scheduling.
    Node* use = edge.from();
    if (IrOpcode::IsPhiOpcode(use->opcode())) {
      // If the use is from a coupled (i.e. floating) phi, compute the common
      // dominator of its uses. This will not recurse more than one level.
      if (scheduler_->GetPlacement(use) == Scheduler::kCoupled) {
        TRACE("  inspecting uses of coupled #%d:%s\n", use->id(),
              use->op()->mnemonic());
        // TODO(titzer): reenable once above TODO is addressed.
        //        DCHECK_EQ(edge.to(), NodeProperties::GetControlInput(use));
        return GetCommonDominatorOfUses(use);
      }
      // If the use is from a fixed (i.e. non-floating) phi, we use the
      // predecessor block of the corresponding control input to the merge.
      if (scheduler_->GetPlacement(use) == Scheduler::kFixed) {
        TRACE("  input@%d into a fixed phi #%d:%s\n", edge.index(), use->id(),
              use->op()->mnemonic());
        Node* merge = NodeProperties::GetControlInput(use, 0);
        DCHECK(IrOpcode::IsMergeOpcode(merge->opcode()));
        Node* input = NodeProperties::GetControlInput(merge, edge.index());
        return FindPredecessorBlock(input);
      }
    } else if (IrOpcode::IsMergeOpcode(use->opcode())) {
      // If the use is from a fixed (i.e. non-floating) merge, we use the
      // predecessor block of the current input to the merge.
      if (scheduler_->GetPlacement(use) == Scheduler::kFixed) {
        TRACE("  input@%d into a fixed merge #%d:%s\n", edge.index(), use->id(),
              use->op()->mnemonic());
        return FindPredecessorBlock(edge.to());
      }
    }
    BasicBlock* result = schedule_->block(use);
    if (result == nullptr) return nullptr;
    TRACE("  must dominate use #%d:%s in id:%d\n", use->id(),
          use->op()->mnemonic(), result->id().ToInt());
    return result;
  }

  void ScheduleFloatingControl(BasicBlock* block, Node* node) {
    scheduler_->FuseFloatingControl(block, node);
  }

  void ScheduleRegion(BasicBlock* block, Node* region_end) {
    // We only allow regions of instructions connected into a linear
    // effect chain. The only value allowed to be produced by a node
    // in the chain must be the value consumed by the FinishRegion node.

    // We schedule back to front; we first schedule FinishRegion.
    CHECK_EQ(IrOpcode::kFinishRegion, region_end->opcode());
    ScheduleNode(block, region_end);

    // Schedule the chain.
    Node* node = NodeProperties::GetEffectInput(region_end);
    while (node->opcode() != IrOpcode::kBeginRegion) {
      DCHECK_EQ(0, scheduler_->GetData(node)->unscheduled_count_);
      DCHECK_EQ(1, node->op()->EffectInputCount());
      DCHECK_EQ(1, node->op()->EffectOutputCount());
      DCHECK_EQ(0, node->op()->ControlOutputCount());
      // The value output (if there is any) must be consumed
      // by the EndRegion node.
      DCHECK(node->op()->ValueOutputCount() == 0 ||
             node == region_end->InputAt(0));
      ScheduleNode(block, node);
      node = NodeProperties::GetEffectInput(node);
    }
    // Schedule the BeginRegion node.
    DCHECK_EQ(0, scheduler_->GetData(node)->unscheduled_count_);
    ScheduleNode(block, node);
  }

  void ScheduleNode(BasicBlock* block, Node* node) {
    schedule_->PlanNode(block, node);
    size_t block_id = block->id().ToSize();
    if (!scheduler_->scheduled_nodes_[block_id]) {
      scheduler_->scheduled_nodes_[block_id] =
          new (zone_->New(sizeof(NodeVector))) NodeVector(zone_);
    }
    scheduler_->scheduled_nodes_[block_id]->push_back(node);
    scheduler_->UpdatePlacement(node, Scheduler::kScheduled);
  }

  Node* CloneNode(Node* node) {
    int const input_count = node->InputCount();
    for (int index = 0; index < input_count; ++index) {
      Node* const input = node->InputAt(index);
      scheduler_->IncrementUnscheduledUseCount(input, index, node);
    }
    Node* const copy = scheduler_->graph_->CloneNode(node);
    TRACE(("clone #%d:%s -> #%d\n"), node->id(), node->op()->mnemonic(),
          copy->id());
    scheduler_->node_data_.resize(copy->id() + 1,
                                  scheduler_->DefaultSchedulerData());
    scheduler_->node_data_[copy->id()] = scheduler_->node_data_[node->id()];
    return copy;
  }

  Zone* zone_;
  Scheduler* scheduler_;
  Schedule* schedule_;
  BoolVector marked_;
  ZoneDeque<BasicBlock*> marking_queue_;
};


void Scheduler::ScheduleLate() {
  TRACE("--- SCHEDULE LATE ------------------------------------------\n");
  if (FLAG_trace_turbo_scheduler) {
    TRACE("roots: ");
    for (Node* node : schedule_root_nodes_) {
      TRACE("#%d:%s ", node->id(), node->op()->mnemonic());
    }
    TRACE("\n");
  }

  // Schedule: Places nodes in dominator block of all their uses.
  ScheduleLateNodeVisitor schedule_late_visitor(zone_, this);
  schedule_late_visitor.Run(&schedule_root_nodes_);
}


// -----------------------------------------------------------------------------
// Phase 6: Seal the final schedule.


void Scheduler::SealFinalSchedule() {
  TRACE("--- SEAL FINAL SCHEDULE ------------------------------------\n");

  // Serialize the assembly order and reverse-post-order numbering.
  special_rpo_->SerializeRPOIntoSchedule();
  special_rpo_->PrintAndVerifySpecialRPO();

  // Add collected nodes for basic blocks to their blocks in the right order.
  int block_num = 0;
  for (NodeVector* nodes : scheduled_nodes_) {
    BasicBlock::Id id = BasicBlock::Id::FromInt(block_num++);
    BasicBlock* block = schedule_->GetBlockById(id);
    if (nodes) {
      for (Node* node : base::Reversed(*nodes)) {
        schedule_->AddNode(block, node);
      }
    }
  }
}


// -----------------------------------------------------------------------------


void Scheduler::FuseFloatingControl(BasicBlock* block, Node* node) {
  TRACE("--- FUSE FLOATING CONTROL ----------------------------------\n");
  if (FLAG_trace_turbo_scheduler) {
    StdoutStream{} << "Schedule before control flow fusion:\n" << *schedule_;
  }

  // Iterate on phase 1: Build control-flow graph.
  control_flow_builder_->Run(block, node);

  // Iterate on phase 2: Compute special RPO and dominator tree.
  special_rpo_->UpdateSpecialRPO(block, schedule_->block(node));
  // TODO(mstarzinger): Currently "iterate on" means "re-run". Fix that.
  for (BasicBlock* b = block->rpo_next(); b != nullptr; b = b->rpo_next()) {
    b->set_dominator_depth(-1);
    b->set_dominator(nullptr);
  }
  PropagateImmediateDominators(block->rpo_next());

  // Iterate on phase 4: Schedule nodes early.
  // TODO(mstarzinger): The following loop gathering the propagation roots is a
  // temporary solution and should be merged into the rest of the scheduler as
  // soon as the approach settled for all floating loops.
  NodeVector propagation_roots(control_flow_builder_->control_);
  for (Node* node : control_flow_builder_->control_) {
    for (Node* use : node->uses()) {
      if (NodeProperties::IsPhi(use) && IsLive(use)) {
        propagation_roots.push_back(use);
      }
    }
  }
  if (FLAG_trace_turbo_scheduler) {
    TRACE("propagation roots: ");
    for (Node* node : propagation_roots) {
      TRACE("#%d:%s ", node->id(), node->op()->mnemonic());
    }
    TRACE("\n");
  }
  ScheduleEarlyNodeVisitor schedule_early_visitor(zone_, this);
  schedule_early_visitor.Run(&propagation_roots);

  // Move previously planned nodes.
  // TODO(mstarzinger): Improve that by supporting bulk moves.
  scheduled_nodes_.resize(schedule_->BasicBlockCount());
  MovePlannedNodes(block, schedule_->block(node));

  if (FLAG_trace_turbo_scheduler) {
    StdoutStream{} << "Schedule after control flow fusion:\n" << *schedule_;
  }
}


void Scheduler::MovePlannedNodes(BasicBlock* from, BasicBlock* to) {
  TRACE("Move planned nodes from id:%d to id:%d\n", from->id().ToInt(),
        to->id().ToInt());
  NodeVector* from_nodes = scheduled_nodes_[from->id().ToSize()];
  NodeVector* to_nodes = scheduled_nodes_[to->id().ToSize()];
  if (!from_nodes) return;

  for (Node* const node : *from_nodes) {
    schedule_->SetBlockForNode(to, node);
  }
  if (to_nodes) {
    to_nodes->insert(to_nodes->end(), from_nodes->begin(), from_nodes->end());
    from_nodes->clear();
  } else {
    std::swap(scheduled_nodes_[from->id().ToSize()],
              scheduled_nodes_[to->id().ToSize()]);
  }
}

// -----------------------------------------------------------------------------

class LoopRevectorizer : public ZoneObject {
 public:
  LoopRevectorizer(Zone* zone, Scheduler* scheduler)
      : zone_(zone),
        scheduler_(scheduler),
        schedule_(scheduler->schedule_),
        induction_vars_(zone),
        iterator_vars_(zone),
        selected_loop_(zone),
        loop_tree_(nullptr) {}

  InductionVariable* TryGetInductionVariable(Node* phi) {
    DCHECK_EQ(2, phi->op()->ValueInputCount());
    Node* loop = NodeProperties::GetControlInput(phi);
    DCHECK_EQ(IrOpcode::kLoop, loop->opcode());
    Node* initial = phi->InputAt(0);
    Node* arith = phi->InputAt(1);
    InductionVariable::ArithmeticType arithmeticType;

    if (arith->opcode() == IrOpcode::kInt32Add ||
        arith->opcode() == IrOpcode::kInt64Add ||
        arith->opcode() == IrOpcode::kFloat32Add ||
        arith->opcode() == IrOpcode::kFloat64Add) {
      arithmeticType = InductionVariable::ArithmeticType::kAddition;
    } else if (arith->opcode() == IrOpcode::kInt32Sub ||
               arith->opcode() == IrOpcode::kInt64Sub ||
               arith->opcode() == IrOpcode::kFloat32Sub ||
               arith->opcode() == IrOpcode::kFloat64Sub) {
      arithmeticType = InductionVariable::ArithmeticType::kSubtraction;
    } else {
      return nullptr;
    }

    Node* input = arith->InputAt(0);

    if (input != phi) return nullptr;

    Node* effect_phi = nullptr;
    for (Node* use : loop->uses()) {
      if (use->opcode() == IrOpcode::kEffectPhi) {
        DCHECK_NULL(effect_phi);
        effect_phi = use;
      }
    }
    if (!effect_phi) return nullptr;

    Node* incr = arith->InputAt(1);

    TRACE(
        "revec--- induction var: phi %i, effect_phi %i, arith %i, incr "
        "%i, init %i, type %i\n",
        phi->id(), effect_phi->id(), arith->id(), incr->id(), initial->id(),
        arithmeticType);
    return new (zone_) InductionVariable(phi, effect_phi, arith, incr, initial,
                                         zone_, arithmeticType);
  }

  void DetectInductionVariables(Node* loop) {
    if (loop->op()->ControlInputCount() != 2) return;
    TRACE("revec--- Loop variables for loop %i:\n", loop->id());
    for (Edge edge : loop->use_edges()) {
      if (NodeProperties::IsControlEdge(edge) &&
          edge.from()->opcode() == IrOpcode::kPhi) {
        Node* phi = edge.from();
        InductionVariable* induction_var = TryGetInductionVariable(phi);
        if (induction_var) {
          induction_vars_[phi->id()] = induction_var;
        }
      }
    }
  }

  void TryGetMainIterator(Node* node) {
    Node* branch = node->InputAt(0);
    Node* cond = branch->InputAt(0);
    if (cond->opcode() == IrOpcode::kWord32Equal) {
      if (cond->InputAt(0)->opcode() == IrOpcode::kWord32Equal) {
        cond = cond->InputAt(0);
      }
    } else if (cond->opcode() == IrOpcode::kWord64Equal) {
      if (cond->InputAt(0)->opcode() == IrOpcode::kWord64Equal) {
        cond = cond->InputAt(0);
      }
    }

    Node* left = cond->InputAt(0);
    Node* right = cond->InputAt(1);
    for (auto entry : induction_vars_) {
      Node* final_value = nullptr;
      InductionVariable* induction_var = entry.second;
      if (induction_var->arith() == left) {
        final_value = right;
      } else if (induction_var->arith() == right) {
        final_value = left;
      }

      if (final_value != nullptr) {
        IteratorVariable* iterator_var = new (zone_)
            IteratorVariable(induction_var->phi(), induction_var->effect_phi(),
                             induction_var->arith(), induction_var->increment(),
                             induction_var->init_value(), zone_,
                             induction_var->Type(), cond, final_value);

        iterator_vars_[induction_var->phi()->id()] = iterator_var;
        TRACE("revec--- main induction_var %i\n",
              induction_var->phi()->id());
      }
    }
  }

  void DetectMainIterator(Node* loop) {
    if (loop->op()->ControlInputCount() != 2) return;

    TRACE("revec--- Loop Count for loop %i\n", loop->id());
    int max = NodeProperties::PastControlIndex(loop);
    for (int i = NodeProperties::FirstControlIndex(loop); i < max; i++) {
      Node* input = loop->InputAt(i);
      if (input->opcode() == IrOpcode::kIfFalse ||
          input->opcode() == IrOpcode::kIfTrue) {
        TryGetMainIterator(input);
      }
    }
  }

  bool HasMultipleBackEdge(LoopTree::Loop* loop) {
    Node* loop_node = loop_tree_->GetLoopControl(loop);
    int backedges = loop_node->InputCount() - 1;
    if (backedges > 1) {
      return true;
    }
    return false;
  }

  bool HasUnsupportedOpcode(LoopTree::Loop* loop) {
    bool has_simd = false;
    for (Node* node : loop_tree_->LoopNodes(loop)) {
      if (NodeProperties::IsSimd(node)) {
        has_simd = true;
        TRACE("revec--- found simd node #%d:%s\n", node->id(),
              node->op()->mnemonic());
        if (supported_opcodes_.find(node->opcode()) ==
            supported_opcodes_.end()) {
          TRACE("revec--- unsupported simd node #%d:%s\n", node->id(),
                node->op()->mnemonic());
          return true;
        }
      }
    }
    return !has_simd;
  }

  // Every loop has at least 1 call(Stack Check)
  // TODO: don't check call count, check call parameter
  bool HasAdditionalFunctionCall(LoopTree::Loop* loop) {
    int call_count = 0;
    for (Node* node : loop_tree_->LoopNodes(loop)) {
      if (node->opcode() == IrOpcode::kCall) {
        call_count++;
      }
    }

    return call_count > 1;
  }

  bool IsSupportedConstNode(Node* incr) {
    if (incr->opcode() == IrOpcode::kInt32Constant ||
        incr->opcode() == IrOpcode::kInt64Constant) {
      return true;
    }

    return false;
  }

  bool HasUnsupportedInductionVariables(LoopTree::Loop* loop) {
    for (Node* node : loop_tree_->HeaderNodes(loop)) {
      if (NodeProperties::IsPhi(node)) {
        auto it = induction_vars_.find(node->id());
        if (it != induction_vars_.end()) {
          InductionVariable* var = it->second;
          Node* incr = var->increment();
          if (!IsSupportedConstNode(incr)) {
            return true;
          }
        }
      }
    }

    return false;
  }

  bool IsEven(int64_t number) { return (number & 1) == 0; }

  bool CheckConstIterator(IteratorVariable* var) {
    Node* init = var->init_value();
    Node* incr = var->increment();
    Node* final = var->final_value();
    int64_t init_value = 0;
    int64_t incr_value = 0;
    int64_t final_value = 0;

    IrOpcode::Value op = var->cond()->opcode();

    if (init->opcode() == IrOpcode::kInt32Constant &&
        incr->opcode() == IrOpcode::kInt32Constant &&
        final->opcode() == IrOpcode::kInt32Constant) {
      init_value = OpParameter<int32_t>(init->op());
      incr_value = OpParameter<int32_t>(incr->op());
      final_value = OpParameter<int32_t>(final->op());
    } else if (init->opcode() == IrOpcode::kInt64Constant &&
               incr->opcode() == IrOpcode::kInt64Constant &&
               final->opcode() == IrOpcode::kInt64Constant) {
      init_value = OpParameter<int64_t>(init->op());
      incr_value = OpParameter<int64_t>(incr->op());
      final_value = OpParameter<int64_t>(final->op());
    } else {
      return false;
    }

    if (incr_value == 0) {
      return false;
    }

    int64_t trip_count = 0;
    int64_t reminder = 0;
    if (incr_value > 0) {
      trip_count = (final_value - init_value) / incr_value;
      reminder = (final_value - init_value) % incr_value;
    } else {
      trip_count = (init_value - final_value) / (-incr_value);
      reminder = (init_value - final_value) % (-incr_value);
    }
    if (trip_count < 8) {
      return false;
    }

    if (op == IrOpcode::kWord32Equal || op == IrOpcode::kWord64Equal) {
      if (reminder == 0 && IsEven(trip_count)) {
        return true;
      }
    } else if (op == IrOpcode::kInt32LessThan ||
               op == IrOpcode::kInt64LessThan) {
      if (reminder != 0) {
        trip_count++;
      }
      if (IsEven(trip_count)) {
        return true;
      }
      // TODO: check leftover loop
      if (incr_value < 0 && final_value == (-incr_value) - 1) {
        return true;
      }
    } else if (op == IrOpcode::kInt32LessThanOrEqual ||
               op == IrOpcode::kInt64LessThanOrEqual) {
      // TODO: check leftover loop
      if (incr_value < 0 && final_value == (-incr_value)) {
        return true;
      }
    }

    return false;
  }

  int64_t GetConstantIntValue(Node* node) {
    int64_t value = 0;
    if (node->opcode() == IrOpcode::kInt32Constant) {
      value = OpParameter<int32_t>(node->op());
    } else if (node->opcode() == IrOpcode::kInt64Constant) {
      value = OpParameter<int64_t>(node->op());
    }
    return value;
  }

  bool CheckNonconstIterator(IteratorVariable* var) {
    Node* init = var->init_value();
    Node* incr = var->increment();
    Node* final = var->final_value();

    IrOpcode::Value op = var->cond()->opcode();

    int64_t incr_value = 0;
    if (!NodeProperties::IsConstant(init) &&
        !NodeProperties::IsConstant(final)) {
      // must have >=2 const
      return false;
    }

    incr_value = GetConstantIntValue(incr);

    if (incr_value > 0) {
      if (NodeProperties::IsConstant(init)) {
        if (final->opcode() == IrOpcode::kWord32And) {
          Node* left = final->InputAt(0);
          Node* right = final->InputAt(1);
          Node* para = nullptr;
          Node* mask = nullptr;
          int64_t mask_value = 0;
          if (NodeProperties::IsConstant(left)) {
            para = right;
            mask = left;
          } else if (NodeProperties::IsConstant(right)) {
            para = left;
            mask = right;
          }
          if (mask == nullptr) {
            return false;
          }

          for (Node* use : para->uses()) {
            if (use->opcode() == IrOpcode::kUint32LessThanOrEqual ||
                use->opcode() == IrOpcode::kInt32LessThanOrEqual) {
              Node* leftover = nullptr;
              if (use->InputAt(0) == para &&
                  NodeProperties::IsConstant(use->InputAt(1))) {
                leftover = use->InputAt(1);
              } else if (use->InputAt(1) == para &&
                         NodeProperties::IsConstant(use->InputAt(0))) {
                leftover = use->InputAt(0);
              }

              if (leftover != nullptr) {
                int64_t leftover_value = 0;
                if (leftover->opcode() == IrOpcode::kInt32Constant) {
                  leftover_value = OpParameter<int32_t>(leftover->op());
                  mask_value = OpParameter<int32_t>(mask->op());
                } else if (leftover->opcode() == IrOpcode::kInt64Constant) {
                  leftover_value = OpParameter<int64_t>(leftover->op());
                  mask_value = OpParameter<int64_t>(mask->op());
                }

                if (leftover_value == (~mask_value)) {
                  if (leftover->UseCount() == 1 && mask->UseCount() == 1) {
                    return true;
                  }
                }
              }
            }
          }
        }

        else if (final->opcode() == IrOpcode::kInt32Add) {
            if (op == IrOpcode::kInt32LessThanOrEqual ||
                    op == IrOpcode::kInt64LessThanOrEqual) {
                if(IsSupportedConstNode(final->InputAt(1)) &&
                   (GetConstantIntValue(final->InputAt(1)) == -incr_value))
                {
                    return true;
                }
            }
        }
        /*
            for (Edge edge : node->use_edges()) {
                if (NodeProperties::IsControlEdge(edge)) {
                    Node* marker = edge.from();
                }
            }
            */
      }

    } else {
    }
    return false;
  }

  bool CheckIterator(IteratorVariable* var) {
    Node* init = var->init_value();
    Node* incr = var->increment();
    Node* final = var->final_value();

    if (NodeProperties::IsConstant(init) && NodeProperties::IsConstant(incr) &&
        NodeProperties::IsConstant(final)) {
      return CheckConstIterator(var);
    } else {
      return CheckNonconstIterator(var);
    }

    return false;
  }

  bool HasUnsupportedMainIterator(LoopTree::Loop* loop) {
    int count = 0;
    for (Node* node : loop_tree_->HeaderNodes(loop)) {
      if (NodeProperties::IsPhi(node)) {
        auto it = iterator_vars_.find(node->id());
        if (it != iterator_vars_.end()) {
          count++;
          IteratorVariable* var = it->second;
          if (!CheckIterator(var)) {
            return true;
          }
          // TODO, OwnedBy
          // if(var->final_value())
        }
      }
    }

    return count != 1;
  }

  // TODO: handle reduction variable
  bool HasOutsideDependency(LoopTree::Loop* loop) {
    std::set<IrOpcode::Value> supported_init_opcode = {
      IrOpcode::kS128Zero,
      IrOpcode::kS128Xor,
      IrOpcode::kF32x4Splat,
      IrOpcode::kI8x16Splat,
      IrOpcode::kI16x8Splat,
      IrOpcode::kI32x4Splat,
    };
    // check input
    // TODO:: make sure node is the only use of input
    for (Node* node : loop_tree_->LoopNodes(loop)) {
      for (Node* input : node->inputs()) {
        if (!loop_tree_->Contains(loop, input)) {  // input of nodes outside loop
          if (NodeProperties::IsSimd(input) &&
              supported_init_opcode.find(input->opcode()) == supported_init_opcode.end()) {
            // BasicBlock* block = schedule_->block(node);
            return true;
          }
        }
      }
    }
    // check uses
    for (Node* node : loop_tree_->LoopNodes(loop)) {
      for (Node* use : node->uses()) {
        if (!loop_tree_->Contains(loop, use)) {  // use of nodes outside loop
          if (NodeProperties::IsSimd(use)) {
            return true;
          }
        }
      }
    }
    return false;
  }

  bool HasMemoryDependency() { return false; }

  bool CanReVectorize(LoopTree::Loop* loop) {
    if (HasMultipleBackEdge(loop)) {
      TRACE("revec--- HasMultipleBackEdge return\n");
      return false;
    }
    if (HasUnsupportedOpcode(loop)) {
      TRACE("revec--- HasUnsupportedOpcode return\n");
      return false;
    }
    if (HasAdditionalFunctionCall(loop)) {
      TRACE("revec--- HasFunctionCall return\n");
      return false;
    }
    if (HasUnsupportedInductionVariables(loop)) {
      TRACE("revec--- HasUnsupportedInductionVariables return\n");
      return false;
    }
    if (HasUnsupportedMainIterator(loop)) {
      TRACE("revec--- HasUnsupportedMainIterator return\n");
      return false;
    }
    if (HasOutsideDependency(loop)) {
      TRACE("revec--- HasOutsideDependency return\n");
      return false;
    }

    // TODO:
    if (HasMemoryDependency()) {
      TRACE("revec--- HasMemoryDependency return");
      return false;
    }

    return true;
  }
#if 0
  // node->param = node->param * multiplier + addend
  void TransformConstantNode(Node* node, int multiplier, int addend) {
    if (!IsSupportedConstNode(node)) {
      TRACE("revec--- can't double non-const node\n");
      return;
    }

    if (node->opcode() == IrOpcode::kInt32Constant) {
      // Operator1<int32_t> * op1 =  dynamic_cast< Operator1<int32_t>* >
      // (node->op());
      Operator1<int32_t>* op1 = (Operator1<int32_t>*)(node->op());
      int32_t value = op1->parameter();
      /*
        int value_out = op1->ValueOutputCount();
        Operator* newop = new (zone_) Operator1<int32_t>(         // --
                IrOpcode::kInt32Constant, Operator::kPure,  // opcode
                "Int32Constant",                            // name
                0, 0, 0, value_out, 0, 0,                           //
        counts,
        TODO, restore value*2) ;                                    //
        parameter NodeProperties::ChangeOp(node,newop);
       */
      op1->SetParameter(value * multiplier + addend);

    } else if (node->opcode() == IrOpcode::kInt64Constant) {
      // Operator1<int64_t> * op1 =  dynamic_cast< Operator1<int64_t>* >
      // (node->op());
      Operator1<int64_t>* op1 = (Operator1<int64_t>*)(node->op());
      int64_t value = op1->parameter();

      /*
      int value_out = op1->ValueOutputCount();
      NodeProperties::ChangeOp(node,
                               new (zone_) Operator1<int64_t>(         //
      -- IrOpcode::kInt64Constant, Operator::kPure,  // opcode
      "Int64Constant", // name 0, 0, 0, value_out, 0, 0, // counts
      value*2) // parameter
                               );
                               */
      op1->SetParameter(value * multiplier + addend);
    } else {
      TRACE("revec--- Int64 and Int32 const only!, unimplement\n");
    }
  }
#endif

  // new_node->param = old_node->param * multiplier + addend
  Node* CreateConstantNode(Node* old_node, int multiplier, int addend) {
    Node* new_node = nullptr;
    if (!IsSupportedConstNode(old_node)) {
      TRACE("revec--- can't double non-const node\n");
      return nullptr;
    }
    if (old_node->opcode() == IrOpcode::kInt32Constant) {
      Operator1<int32_t>* op1 = (Operator1<int32_t>*)(old_node->op());
      int32_t value = op1->parameter();
      new_node =
          scheduler_->mcgraph_->Int32Constant(value * multiplier + addend);

    } else if (old_node->opcode() == IrOpcode::kInt64Constant) {
      Operator1<int64_t>* op1 = (Operator1<int64_t>*)(old_node->op());
      int64_t value = op1->parameter();
      new_node =
          scheduler_->mcgraph_->Int64Constant(value * multiplier + addend);
    }

    return new_node;
  }

  void UpdateInductionVariableStride(LoopTree::Loop* loop) {
    for (Node* node : loop_tree_->HeaderNodes(loop)) {
      if (NodeProperties::IsPhi(node)) {
        auto it = induction_vars_.find(node->id());
        if (it != induction_vars_.end()) {
          InductionVariable* var = it->second;
          Node* incr = var->increment();
          Node* arith = var->arith();
          if (IsSupportedConstNode(incr)) {
            Node* new_incr = CreateConstantNode(incr, 2, 0);

            scheduler_->node_data_.resize(scheduler_->node_data_.size()+1,//increase size
                                          scheduler_->DefaultSchedulerData());
            scheduler_->node_data_[new_incr->id()] =
                scheduler_->node_data_[incr->id()];

            arith->ReplaceInput(1, new_incr);
            TRACE("revec--- create constant node %d->%d\n", incr->id(),
                  new_incr->id());
          }
        }
      }
    }
  }

  void UpdateIteratorCond(IteratorVariable* var) {
    Node* init = var->init_value();
    Node* incr = var->increment();
    Node* final = var->final_value();
    int64_t init_value = 0;
    int64_t incr_value = 0;
    int64_t final_value = 0;

    Node* cond = var->cond();
    IrOpcode::Value op = var->cond()->opcode();

    if (!NodeProperties::IsConstant(init) ||
        !NodeProperties::IsConstant(incr) ||
        !NodeProperties::IsConstant(final)) {
      return;
    }
    if (init->opcode() == IrOpcode::kInt32Constant &&
        incr->opcode() == IrOpcode::kInt32Constant &&
        final->opcode() == IrOpcode::kInt32Constant) {
      init_value = OpParameter<int32_t>(init->op());
      incr_value = OpParameter<int32_t>(incr->op());
      final_value = OpParameter<int32_t>(final->op());
    } else if (init->opcode() == IrOpcode::kInt64Constant &&
               incr->opcode() == IrOpcode::kInt64Constant &&
               final->opcode() == IrOpcode::kInt64Constant) {
      init_value = OpParameter<int64_t>(init->op());
      incr_value = OpParameter<int64_t>(incr->op());
      final_value = OpParameter<int64_t>(final->op());
    }

    if (incr_value == 0) {
      return;
    }

    if (incr_value > 0) {
      if (cond->InputAt(1) != final) {
        return;
      }

      if (op == IrOpcode::kWord32Equal) {
      } else if (op == IrOpcode::kInt32LessThan) {
      } else if (op == IrOpcode::kInt32LessThanOrEqual) {
      }
    } else {  //<0 only, =0 is exclued

      if (cond->InputAt(0) != final) {
        return;
      }

      if (op == IrOpcode::kWord32Equal) {
      } else if (op == IrOpcode::kInt32LessThan) {
        if (final_value == (-incr_value) - 1) {
          //TransformConstantNode(final, 2, 1);
          Node* newfinal = CreateConstantNode(final, 2, 1);
          if(cond->InputAt(0) == final)
          {
            cond->ReplaceInput(0, newfinal);
          }
          else
          {
            cond->ReplaceInput(1, newfinal);
          }

        }
      } else if (op == IrOpcode::kInt32LessThanOrEqual) {
        if (final_value == (-incr_value)) {
          //TransformConstantNode(final, 2, 0);
          Node* newfinal = CreateConstantNode(final, 2, 0);
          if(cond->InputAt(0) == final)
          {
            cond->ReplaceInput(0, newfinal);
          }
          else
          {
            cond->ReplaceInput(1, newfinal);
          }

        }
      }
    }
  }

  void UpdateIterator(LoopTree::Loop* loop) {
    for (Node* node : loop_tree_->HeaderNodes(loop)) {
      if (NodeProperties::IsPhi(node)) {
        auto it = iterator_vars_.find(node->id());
        if (it != iterator_vars_.end()) {
          IteratorVariable* var = it->second;
          UpdateIteratorCond(var);
        }
      }
    }
  }

  void UpdateGraph(LoopTree::Loop* loop) {
    UpdateIterator(loop);
    UpdateInductionVariableStride(loop);
  }

  void ReVectorizeIfPossible(LoopTree::Loop* loop) {
    Node* loop_node = loop_tree_->GetLoopControl(loop);

    TRACE("revec--- try to revec loop %i:\n", loop_node->id());
    DetectInductionVariables(loop_node);
    DetectMainIterator(loop_node);
    if (CanReVectorize(loop)) {
      selected_loop_.insert(loop);
      UpdateGraph(loop);
      TRACE("revec--- finish revec loop %i:\n\n", loop_node->id());
      //PrintF("revec--- finish revec loop %i:\n\n", loop_node->id());
    } else {
      TRACE("revec--- can't revec loop %i:\n\n", loop_node->id());
    }
  }

  void ReVectorizeInnerLoops(LoopTree::Loop* loop) {
    // If the loop has nested loops, revectorize inside those.
    if (!loop->children().empty()) {
      for (LoopTree::Loop* inner_loop : loop->children()) {
        ReVectorizeInnerLoops(inner_loop);
      }
      return;
    }
    // Only revectorize small-enough loops.
    if (loop->TotalSize() > kMaxLoopNodes) return;

    ReVectorizeIfPossible(loop);
  }

  void SelectLoopAndUpdateGraph() {
    loop_tree_ = LoopFinder::BuildLoopTree(scheduler_->graph_, scheduler_->tick_counter_, zone_);
    for (LoopTree::Loop* loop : loop_tree_->outer_loops()) {
      ReVectorizeInnerLoops(loop);
    }
  }

  BasicBlock* BasicBlockForLoopHeader(LoopTree::Loop* loop) {
    Node* loop_node = loop_tree_->GetLoopControl(loop);

    BasicBlock* header = nullptr;

    BasicBlockVector* blocks = schedule_->rpo_order();
    for (auto const block : *blocks) {
      if (block->IsLoopHeader()) {
        DCHECK_LE(2u, block->PredecessorCount());

        for (Node* const node : *block) {
          if (node->opcode() == IrOpcode::kLoop && node == loop_node) {
            header = block;
            TRACE("revec--- block id:%d for loop node #%d:%s\n",
                  header->rpo_number(), node->id(), node->op()->mnemonic());
            return header;
          }
        }
      }
    }
    return header;
  }

  void MarkBlockInLoop(LoopTree::Loop* loop) {
    BasicBlockVector* blocks = schedule_->rpo_order();

    BasicBlock* header = BasicBlockForLoopHeader(loop);
    if (header != nullptr) {
      for (auto block : *blocks) {
        if (header->LoopContains(block)) {
          TRACE("revec--- mark block id:%d for loop convert\n",
                block->rpo_number());
          block->set_need_convert(true);
        }
      }
    }
  }

  // TODO: handle reduction variable
  void MarkBlockOutLoop(LoopTree::Loop* loop) {

    TRACE("revec--- enter %s\n", __func__);
    std::set<IrOpcode::Value> supported_init_opcode = {
      IrOpcode::kS128Zero,
      IrOpcode::kS128Xor,
      IrOpcode::kF32x4Splat,
      IrOpcode::kI8x16Splat,
      IrOpcode::kI16x8Splat,
      IrOpcode::kI32x4Splat,
    };
    // check input
    for (Node* node : loop_tree_->LoopNodes(loop)) {
      for (Node* input : node->inputs()) {
        if (!loop_tree_->Contains(loop, input)) {  // input of nodes outside loop
          if (NodeProperties::IsSimd(input) &&
              supported_init_opcode.find(input->opcode()) != supported_init_opcode.end()) {
              BasicBlock* block = schedule_->block(input);

              TRACE("revec--- mark out block id:%d for loop convert\n",
                  block->rpo_number());
              block->set_need_convert(true);
          }
        }
      }
    }
  }

  void MarkBlockInLoops() {
    for (auto it = selected_loop_.begin(); it != selected_loop_.end(); ++it) {
      MarkBlockInLoop(*it);
      MarkBlockOutLoop(*it);
    }
  }

 private:
  Zone* zone_;
  Scheduler* scheduler_;
  Schedule* schedule_;
  ZoneMap<int, InductionVariable*> induction_vars_;
  ZoneMap<int, IteratorVariable*> iterator_vars_;
  ZoneSet<LoopTree::Loop*> selected_loop_;
  LoopTree* loop_tree_;

  static const size_t kMaxLoopNodes = 1000;
  static const std::set<IrOpcode::Value> supported_opcodes_;
};

const std::set<IrOpcode::Value> LoopRevectorizer::supported_opcodes_ = {
    /*
    IrOpcode::kF32x4Splat,
    IrOpcode::kF32x4ExtractLane,
    IrOpcode::kF32x4ReplaceLane,
    IrOpcode::kF32x4SConvertI32x4,
    IrOpcode::kF32x4UConvertI32x4,
    IrOpcode::kF32x4Abs,
    IrOpcode::kF32x4Neg,
    IrOpcode::kF32x4RecipApprox,
    IrOpcode::kF32x4RecipSqrtApprox,
  */
    IrOpcode::kF32x4Add,
    /*
    IrOpcode::kF32x4AddHoriz,
    IrOpcode::kF32x4Sub,
     */
    IrOpcode::kF32x4Mul,
  /*
    IrOpcode::kF32x4Min,
    IrOpcode::kF32x4Max,
    IrOpcode::kF32x4Eq,
    IrOpcode::kF32x4Ne,
    IrOpcode::kF32x4Lt,
    IrOpcode::kF32x4Le,
    IrOpcode::kF32x4Gt,
    IrOpcode::kF32x4Ge,
    IrOpcode::kI32x4Splat,
    IrOpcode::kI32x4ExtractLane,
    IrOpcode::kI32x4ReplaceLane,
    IrOpcode::kI32x4SConvertF32x4,
    IrOpcode::kI32x4SConvertI16x8Low,
    IrOpcode::kI32x4SConvertI16x8High,
    IrOpcode::kI32x4Neg,
    IrOpcode::kI32x4Shl,
    IrOpcode::kI32x4ShrS,
      */
    IrOpcode::kI32x4Add,
  /*
    IrOpcode::kI32x4AddHoriz,
    IrOpcode::kI32x4Sub,
    IrOpcode::kI32x4Mul,
    IrOpcode::kI32x4MinS,
    IrOpcode::kI32x4MaxS,
    IrOpcode::kI32x4Eq,
    IrOpcode::kI32x4Ne,
    IrOpcode::kI32x4LtS,
    IrOpcode::kI32x4LeS,
    IrOpcode::kI32x4GtS,
    IrOpcode::kI32x4GeS,
    IrOpcode::kI32x4UConvertF32x4,
    IrOpcode::kI32x4UConvertI16x8Low,
    IrOpcode::kI32x4UConvertI16x8High,
    IrOpcode::kI32x4ShrU,
    IrOpcode::kI32x4MinU,
    IrOpcode::kI32x4MaxU,
    IrOpcode::kI32x4LtU,
    IrOpcode::kI32x4LeU,
    IrOpcode::kI32x4GtU,
    IrOpcode::kI32x4GeU,
    IrOpcode::kI16x8Splat,
    IrOpcode::kI16x8ExtractLane,
    IrOpcode::kI16x8ReplaceLane,
    IrOpcode::kI16x8SConvertI8x16Low,
    IrOpcode::kI16x8SConvertI8x16High,
    IrOpcode::kI16x8Neg,
    IrOpcode::kI16x8Shl,
    IrOpcode::kI16x8ShrS,
    IrOpcode::kI16x8SConvertI32x4,
    IrOpcode::kI16x8Add,
    IrOpcode::kI16x8AddSaturateS,
    IrOpcode::kI16x8AddHoriz,
    IrOpcode::kI16x8Sub,
    IrOpcode::kI16x8SubSaturateS,
    IrOpcode::kI16x8Mul,
    IrOpcode::kI16x8MinS,
    IrOpcode::kI16x8MaxS,
    IrOpcode::kI16x8Eq,
    IrOpcode::kI16x8Ne,
    IrOpcode::kI16x8LtS,
    IrOpcode::kI16x8LeS,
    IrOpcode::kI16x8GtS,
    IrOpcode::kI16x8GeS,
    IrOpcode::kI16x8UConvertI8x16Low,
    IrOpcode::kI16x8UConvertI8x16High,
    IrOpcode::kI16x8ShrU,
    IrOpcode::kI16x8UConvertI32x4,
    IrOpcode::kI16x8AddSaturateU,
    IrOpcode::kI16x8SubSaturateU,
    IrOpcode::kI16x8MinU,
    IrOpcode::kI16x8MaxU,
    IrOpcode::kI16x8LtU,
    IrOpcode::kI16x8LeU,
    IrOpcode::kI16x8GtU,
    IrOpcode::kI16x8GeU,
    IrOpcode::kI8x16Splat,
    IrOpcode::kI8x16ExtractLane,
    IrOpcode::kI8x16ReplaceLane,
    IrOpcode::kI8x16SConvertI16x8,
    IrOpcode::kI8x16Neg,
    IrOpcode::kI8x16Shl,
    IrOpcode::kI8x16ShrS,
    IrOpcode::kI8x16Add,
    IrOpcode::kI8x16AddSaturateS,
    IrOpcode::kI8x16Sub,
    IrOpcode::kI8x16SubSaturateS,
    IrOpcode::kI8x16Mul,
    IrOpcode::kI8x16MinS,
    IrOpcode::kI8x16MaxS,
    IrOpcode::kI8x16Eq,
    IrOpcode::kI8x16Ne,
    IrOpcode::kI8x16LtS,
    IrOpcode::kI8x16LeS,
    IrOpcode::kI8x16GtS,
    IrOpcode::kI8x16GeS,
    IrOpcode::kI8x16UConvertI16x8,
    IrOpcode::kI8x16AddSaturateU,
    IrOpcode::kI8x16SubSaturateU,
    IrOpcode::kI8x16ShrU,
    IrOpcode::kI8x16MinU,
    IrOpcode::kI8x16MaxU,
    IrOpcode::kI8x16LtU,
    IrOpcode::kI8x16LeU,
    */
    IrOpcode::kI8x16GtU,
    IrOpcode::kI8x16GeU,
    /*
    IrOpcode::kS128Load,
    IrOpcode::kS128Store,
    IrOpcode::kS128Zero,
    IrOpcode::kS128Not,
    */
    IrOpcode::kS128And,
    /*
    IrOpcode::kS128Or,
    IrOpcode::kS128Xor,
    IrOpcode::kS128Select,
    IrOpcode::kS8x16Shuffle,
    IrOpcode::kS1x4AnyTrue,
    IrOpcode::kS1x4AllTrue,
    IrOpcode::kS1x8AnyTrue,
    IrOpcode::kS1x8AllTrue,
    IrOpcode::kS1x16AnyTrue,
    IrOpcode::kS1x16AllTrue,
    */
};

// -----------------------------------------------------------------------------
// Phase 7:
void Scheduler::SelectLoopAndUpdateGraph() {
  TRACE("--- %s -------------------------------------------\n", __func__);
  loop_revectorizer_ = new (zone_) LoopRevectorizer(zone_, this);
  loop_revectorizer_->SelectLoopAndUpdateGraph();
}
// Phase 7:
void Scheduler::MarkBlockInLoops() {
  TRACE("--- %s -------------------------------------------\n", __func__);
  loop_revectorizer_->MarkBlockInLoops();
}

#undef TRACE

}  // namespace compiler
}  // namespace internal
}  // namespace v8
