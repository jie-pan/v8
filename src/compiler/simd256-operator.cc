#include "src/compiler/simd256-operator.h"

namespace v8 {
namespace internal {
namespace compiler {

// The format is:
// V(Name, properties, value_input_count, control_input_count, output_count)
#define MACHINE_PURE_OP_LIST(V)                   \
  V(F32x8Add, Operator::kCommutative, 2, 0, 1)    \
  V(F32x8Sub, Operator::kNoProperties, 2, 0, 1)   \
  V(F32x8Mul, Operator::kCommutative, 2, 0, 1)    \
  V(F32x8Div, Operator::kNoProperties, 2, 0, 1)   \
  V(F32x8Min, Operator::kCommutative, 2, 0, 1)    \
  V(F32x8Max, Operator::kCommutative, 2, 0, 1)    \
  V(F32x8Eq, Operator::kCommutative, 2, 0, 1)     \
  V(F32x8Ne, Operator::kCommutative, 2, 0, 1)     \
  V(F32x8Lt, Operator::kNoProperties, 2, 0, 1)    \
  V(F32x8Le, Operator::kNoProperties, 2, 0, 1)    \
  V(S256Select, Operator::kNoProperties, 3, 0, 1)

struct Simd256OperatorGlobalCache final {
#define PURE(Name, properties, value_input_count, control_input_count,         \
             output_count)                                                     \
  struct Name##Operator final : public Operator {                              \
    Name##Operator()                                                           \
        : Operator(IrOpcode::k##Name, Operator::kPure | properties, #Name,     \
                   value_input_count, 0, control_input_count, output_count, 0, \
                   0) {}                                                       \
  };                                                                           \
  Name##Operator k##Name;
  MACHINE_PURE_OP_LIST(PURE)

#undef PURE
};

namespace {
DEFINE_LAZY_LEAKY_OBJECT_GETTER(Simd256OperatorGlobalCache,
                                GetSimd256OperatorGlobalCache)
}

Simd256OperatorBuilder::Simd256OperatorBuilder(Zone* zone)
    : cache_(*GetSimd256OperatorGlobalCache()), zone_(zone) {}

#define PURE(Name, properties, value_input_count, control_input_count, \
             output_count)                                             \
  const Operator* Simd256OperatorBuilder::Name() { return &cache_.k##Name; }
MACHINE_PURE_OP_LIST(PURE)
#undef PURE

#undef MACHINE_PURE_OP_LIST
}  // namespace compiler
}  // namespace internal
}  // namespace v8
