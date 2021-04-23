
#ifndef V8_COMPILER_SIMD_WIDENING_H_
#define V8_COMPILER_SIMD_WIDENING_H_

#include "src/compiler/common-operator.h"
#include "src/compiler/diamond.h"
#include "src/compiler/graph.h"
#include "src/compiler/machine-graph.h"
#include "src/compiler/machine-operator.h"
#include "src/compiler/node-marker.h"
#include "src/compiler/simd256-operator.h"
#include "src/compiler/simplified-operator.h"
#include "src/zone/zone-containers.h"

namespace v8 {
namespace internal {

template <typename T>
class Signature;

namespace compiler {

class V8_EXPORT_PRIVATE VectorPromotion {
 public:
  VectorPromotion(MachineGraph* mcgraph, Simd256OperatorBuilder* simd256,
                  Signature<MachineRepresentation>* signature);

  void PromoteGraph();

 private:
  enum class State : uint8_t { kUnvisited, kOnStack, kVisited };
  struct NodeState {
    Node* node;
    int input_index;
  };

  Zone* zone() const { return mcgraph_->zone(); }
  Graph* graph() const { return mcgraph_->graph(); }
  MachineOperatorBuilder* machine() const { return mcgraph_->machine(); }
  CommonOperatorBuilder* common() const { return mcgraph_->common(); }

  void PromoteNode(Node* node);
  void PromoteLoadNode(Node* node);
  void PromoteStoreNode(Node* node);
  void PromoteLoadTransformNode(Node* node);
  void ReplaceSimdNodes();

  MachineGraph* const mcgraph_;
  Simd256OperatorBuilder* const simd256_;
  NodeMarker<State> state_;
  ZoneDeque<NodeState> stack_;
  ZoneMap<Node*, const Operator*> replacements_;
  Signature<MachineRepresentation>* signature_;
  bool has_unsupported_simd_op_;
};

}  // namespace compiler
}  // namespace internal
}  // namespace v8

#endif
