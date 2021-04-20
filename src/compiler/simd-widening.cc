#include "src/compiler/simd-widening.h"

namespace v8 {
namespace internal {
namespace compiler {

#define TRACE(...)                                       \
  do {                                                   \
    if (1) {                                             \
        PrintF("simdwidening:");                         \
        PrintF(__VA_ARGS__);                             \
    }                                                    \
  } while (false)

SimdWidening::SimdWidening(
    MachineGraph* mcgraph, Simd256OperatorBuilder* simd256,
    Signature<MachineRepresentation>* signature)
    : mcgraph_(mcgraph),
      simd256_(simd256),
      state_(mcgraph->graph(), 3),
      stack_(mcgraph_->zone()),
      signature_(signature) {
  DCHECK_NOT_NULL(graph());
  DCHECK_NOT_NULL(graph()->end());
}

void SimdWidening::LowerGraph() {
  stack_.push_back({graph()->end(), 0});
  state_.Set(graph()->end(), State::kOnStack);
  //replacements_[graph()->end()->id()].type = SimdType::kInt32x4;

  while (!stack_.empty()) {
    NodeState& top = stack_.back();
    if (top.input_index == top.node->InputCount()) {
      // All inputs of top have already been lowered, now lower top.
      stack_.pop_back();
      state_.Set(top.node, State::kVisited);
      LowerNode(top.node);
    } else {
      // Push the next input onto the stack.
      Node* input = top.node->InputAt(top.input_index++);
      if (state_.Get(input) == State::kUnvisited) {
        //SetLoweredType(input, top.node);
        if (input->opcode() == IrOpcode::kPhi) {
          // To break cycles with phi nodes we push phis on a separate stack so
          // that they are processed after all other nodes.
          //PreparePhiReplacement(input);
          stack_.push_front({input, 0});
        } else if (input->opcode() == IrOpcode::kEffectPhi ||
                   input->opcode() == IrOpcode::kLoop) {
          stack_.push_front({input, 0});
        } else {
          stack_.push_back({input, 0});
        }
        state_.Set(input, State::kOnStack);
      }
    }
  }
}

void SimdWidening::LowerLoadNode(Node* node) {
  MachineRepresentation rep = LoadRepresentationOf(node->op()).representation();
  if (rep != MachineRepresentation::kSimd128) {
    return;
  }
  constexpr MachineType type = MachineType::Simd256();
  const Operator* op = nullptr;
  switch (node->opcode()) {
    case IrOpcode::kLoad: {
      op = machine()->Load(type);
      break;
    }
    case IrOpcode::kProtectedLoad: {
      op = machine()->ProtectedLoad(type);
      break;
    }

    case IrOpcode::kUnalignedLoad: {
      op = machine()->UnalignedLoad(type);
      break;
    }
    case IrOpcode::kLoadImmutable: {
      op = machine()->LoadImmutable(type);
      break;
    }
    default:
      break;
  }
  if(op != nullptr) {
    TRACE("#%d:%s\n", node->id(),node->op()->mnemonic());
    NodeProperties::ChangeOp(node, op);
  }
}

void SimdWidening::LowerStoreNode(Node* node) {
  DCHECK_LT(2, node->InputCount());

  const Operator* store_op = nullptr;
  MachineRepresentation rep;
  switch (node->opcode()) {
    case IrOpcode::kStore: {
      rep = StoreRepresentationOf(node->op()).representation();
      if(rep == MachineRepresentation::kSimd128) {
        rep = MachineRepresentation::kSimd256;
      }
      WriteBarrierKind write_barrier_kind =
          StoreRepresentationOf(node->op()).write_barrier_kind();
      store_op = machine()->Store(StoreRepresentation(
          rep, write_barrier_kind));
      break;
    }
    case IrOpcode::kUnalignedStore: {
      rep = UnalignedStoreRepresentationOf(node->op());
      if(rep == MachineRepresentation::kSimd128) {
        rep = MachineRepresentation::kSimd256;
      }

      store_op = machine()->UnalignedStore(rep);
      break;
    }
    case IrOpcode::kProtectedStore: {
      rep = StoreRepresentationOf(node->op()).representation();
      if(rep == MachineRepresentation::kSimd128) {
        rep = MachineRepresentation::kSimd256;
      }
      store_op = machine()->ProtectedStore(rep);
      break;
    }
    default:
      break;
  }
  if(store_op != nullptr) {
    TRACE("#%d:%s\n", node->id(),node->op()->mnemonic());
    NodeProperties::ChangeOp(node, store_op);
  }
}



void SimdWidening::LowerLoadTransformNode(Node* node) {
  const Operator* op;
  LoadTransformation transform;

  LoadTransformParameters params = LoadTransformParametersOf(node->op());

  switch (params.transformation) {
    case LoadTransformation::kS128Load8Splat:
      //replacements_[node->id()].type = SimdType::kInt8x16;
      break;
    case LoadTransformation::kS128Load16Splat:
    case LoadTransformation::kS128Load8x8S:
    case LoadTransformation::kS128Load8x8U:
      //replacements_[node->id()].type = SimdType::kInt16x8;
      break;
    case LoadTransformation::kS128Load32Splat:
      transform =  LoadTransformation::kS256Load32Splat;
      op = machine()->LoadTransform(params.kind, transform);
      NodeProperties::ChangeOp(node, op);
      break;
    case LoadTransformation::kS128Load16x4S:
    case LoadTransformation::kS128Load16x4U:
    case LoadTransformation::kS128Load32Zero:
      //replacements_[node->id()].type = SimdType::kInt32x4;
      break;
  case LoadTransformation::kS128Load64Splat:
  case LoadTransformation::kS128Load32x2S:
  case LoadTransformation::kS128Load32x2U:
  case LoadTransformation::kS128Load64Zero:
      //replacements_[node->id()].type = SimdType::kInt64x2;
      break;
  default:
    break;
  }

}

void SimdWidening::LowerNode(Node* node) {
  //if
  const Operator* op;

  switch (node->opcode()) {
    case IrOpcode::kF32x4Add: 
      //DCHECK_EQ(2, node->InputCount());
      op = simd256_->F32x8Add();
      NodeProperties::ChangeOp(node, op);
      break;

    case IrOpcode::kF32x4Mul:
      op = simd256_->F32x8Mul();
      NodeProperties::ChangeOp(node, op);
      break;

    case IrOpcode::kF32x4Min:
      op = simd256_->F32x8Min();
      NodeProperties::ChangeOp(node, op);
      break;

    case IrOpcode::kF32x4Max:
      op = simd256_->F32x8Max();
      NodeProperties::ChangeOp(node, op);
      break;

    case IrOpcode::kF32x4Lt:
      op = simd256_->F32x8Lt();
      NodeProperties::ChangeOp(node, op);
      break;

    case IrOpcode::kF32x4Le:
      op = simd256_->F32x8Le();
      NodeProperties::ChangeOp(node, op);
      break;

    case IrOpcode::kS128Select:
      op = simd256_->S256Select();
      NodeProperties::ChangeOp(node, op);
      break;

    case IrOpcode::kLoad:
    case IrOpcode::kProtectedLoad:
    case IrOpcode::kUnalignedLoad:
    case IrOpcode::kLoadImmutable:
      LowerLoadNode(node);
      break;

    case IrOpcode::kStore:
    case IrOpcode::kUnalignedStore:
    case IrOpcode::kProtectedStore:
      LowerStoreNode(node);
      break;
    case IrOpcode::kLoadTransform:
     LowerLoadTransformNode(node);
     break;

    default:
      break;
  }
}

 

}  // namespace compiler
}  // namespace internal
}  // namespace v8
