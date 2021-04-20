#include "src/compiler/simd256-operator.h"

namespace v8 {
namespace internal {
namespace compiler {

#if 0
// The format is:
// V(Name, properties, value_input_count, control_input_count, output_count)
#define MACHINE_PURE_OP_LIST(V)                                            \
  V(Simd128ReverseBytes, Operator::kNoProperties, 1, 0, 1)                 \
  V(F64x2Splat, Operator::kNoProperties, 1, 0, 1)                          \
  V(F64x2Abs, Operator::kNoProperties, 1, 0, 1)                            \
  V(F64x2Neg, Operator::kNoProperties, 1, 0, 1)                            \
  V(F64x2Sqrt, Operator::kNoProperties, 1, 0, 1)                           \
  V(F64x2Add, Operator::kCommutative, 2, 0, 1)                             \
  V(F64x2Sub, Operator::kNoProperties, 2, 0, 1)                            \
  V(F64x2Mul, Operator::kCommutative, 2, 0, 1)                             \
  V(F64x2Div, Operator::kNoProperties, 2, 0, 1)                            \
  V(F64x2Min, Operator::kCommutative, 2, 0, 1)                             \
  V(F64x2Max, Operator::kCommutative, 2, 0, 1)                             \
  V(F64x2Eq, Operator::kCommutative, 2, 0, 1)                              \
  V(F64x2Ne, Operator::kCommutative, 2, 0, 1)                              \
  V(F64x2Lt, Operator::kNoProperties, 2, 0, 1)                             \
  V(F64x2Le, Operator::kNoProperties, 2, 0, 1)                             \
  V(F64x2Qfma, Operator::kNoProperties, 3, 0, 1)                           \
  V(F64x2Qfms, Operator::kNoProperties, 3, 0, 1)                           \
  V(F64x2Pmin, Operator::kNoProperties, 2, 0, 1)                           \
  V(F64x2Pmax, Operator::kNoProperties, 2, 0, 1)                           \
  V(F64x2Ceil, Operator::kNoProperties, 1, 0, 1)                           \
  V(F64x2Floor, Operator::kNoProperties, 1, 0, 1)                          \
  V(F64x2Trunc, Operator::kNoProperties, 1, 0, 1)                          \
  V(F64x2NearestInt, Operator::kNoProperties, 1, 0, 1)                     \
  V(F64x2ConvertLowI32x4S, Operator::kNoProperties, 1, 0, 1)               \
  V(F64x2ConvertLowI32x4U, Operator::kNoProperties, 1, 0, 1)               \
  V(F64x2PromoteLowF32x4, Operator::kNoProperties, 1, 0, 1)                \
  V(F32x4Splat, Operator::kNoProperties, 1, 0, 1)                          \
  V(F32x4SConvertI32x4, Operator::kNoProperties, 1, 0, 1)                  \
  V(F32x4UConvertI32x4, Operator::kNoProperties, 1, 0, 1)                  \
  V(F32x4Abs, Operator::kNoProperties, 1, 0, 1)                            \
  V(F32x4Neg, Operator::kNoProperties, 1, 0, 1)                            \
  V(F32x4Sqrt, Operator::kNoProperties, 1, 0, 1)                           \
  V(F32x4RecipApprox, Operator::kNoProperties, 1, 0, 1)                    \
  V(F32x4RecipSqrtApprox, Operator::kNoProperties, 1, 0, 1)                \
  V(F32x4Add, Operator::kCommutative, 2, 0, 1)                             \
  V(F32x4Sub, Operator::kNoProperties, 2, 0, 1)                            \
  V(F32x4Mul, Operator::kCommutative, 2, 0, 1)                             \
  V(F32x4Div, Operator::kNoProperties, 2, 0, 1)                            \
  V(F32x4Min, Operator::kCommutative, 2, 0, 1)                             \
  V(F32x4Max, Operator::kCommutative, 2, 0, 1)                             \
  V(F32x4Eq, Operator::kCommutative, 2, 0, 1)                              \
  V(F32x4Ne, Operator::kCommutative, 2, 0, 1)                              \
  V(F32x4Lt, Operator::kNoProperties, 2, 0, 1)                             \
  V(F32x4Le, Operator::kNoProperties, 2, 0, 1)                             \
  V(F32x4Qfma, Operator::kNoProperties, 3, 0, 1)                           \
  V(F32x4Qfms, Operator::kNoProperties, 3, 0, 1)                           \
  V(F32x4Pmin, Operator::kNoProperties, 2, 0, 1)                           \
  V(F32x4Pmax, Operator::kNoProperties, 2, 0, 1)                           \
  V(F32x4Ceil, Operator::kNoProperties, 1, 0, 1)                           \
  V(F32x4Floor, Operator::kNoProperties, 1, 0, 1)                          \
  V(F32x4Trunc, Operator::kNoProperties, 1, 0, 1)                          \
  V(F32x4NearestInt, Operator::kNoProperties, 1, 0, 1)                     \
  V(F32x4DemoteF64x2Zero, Operator::kNoProperties, 1, 0, 1)                \
  V(I64x2Splat, Operator::kNoProperties, 1, 0, 1)                          \
  V(I64x2SplatI32Pair, Operator::kNoProperties, 2, 0, 1)                   \
  V(I64x2Abs, Operator::kNoProperties, 1, 0, 1)                            \
  V(I64x2Neg, Operator::kNoProperties, 1, 0, 1)                            \
  V(I64x2SConvertI32x4Low, Operator::kNoProperties, 1, 0, 1)               \
  V(I64x2SConvertI32x4High, Operator::kNoProperties, 1, 0, 1)              \
  V(I64x2UConvertI32x4Low, Operator::kNoProperties, 1, 0, 1)               \
  V(I64x2UConvertI32x4High, Operator::kNoProperties, 1, 0, 1)              \
  V(I64x2BitMask, Operator::kNoProperties, 1, 0, 1)                        \
  V(I64x2Shl, Operator::kNoProperties, 2, 0, 1)                            \
  V(I64x2ShrS, Operator::kNoProperties, 2, 0, 1)                           \
  V(I64x2Add, Operator::kCommutative, 2, 0, 1)                             \
  V(I64x2Sub, Operator::kNoProperties, 2, 0, 1)                            \
  V(I64x2Mul, Operator::kCommutative, 2, 0, 1)                             \
  V(I64x2Eq, Operator::kCommutative, 2, 0, 1)                              \
  V(I64x2Ne, Operator::kCommutative, 2, 0, 1)                              \
  V(I64x2GtS, Operator::kNoProperties, 2, 0, 1)                            \
  V(I64x2GeS, Operator::kNoProperties, 2, 0, 1)                            \
  V(I64x2ShrU, Operator::kNoProperties, 2, 0, 1)                           \
  V(I64x2ExtMulLowI32x4S, Operator::kCommutative, 2, 0, 1)                 \
  V(I64x2ExtMulHighI32x4S, Operator::kCommutative, 2, 0, 1)                \
  V(I64x2ExtMulLowI32x4U, Operator::kCommutative, 2, 0, 1)                 \
  V(I64x2ExtMulHighI32x4U, Operator::kCommutative, 2, 0, 1)                \
  V(I32x4Splat, Operator::kNoProperties, 1, 0, 1)                          \
  V(I32x4SConvertF32x4, Operator::kNoProperties, 1, 0, 1)                  \
  V(I32x4SConvertI16x8Low, Operator::kNoProperties, 1, 0, 1)               \
  V(I32x4SConvertI16x8High, Operator::kNoProperties, 1, 0, 1)              \
  V(I32x4Neg, Operator::kNoProperties, 1, 0, 1)                            \
  V(I32x4Shl, Operator::kNoProperties, 2, 0, 1)                            \
  V(I32x4ShrS, Operator::kNoProperties, 2, 0, 1)                           \
  V(I32x4Add, Operator::kCommutative, 2, 0, 1)                             \
  V(I32x4Sub, Operator::kNoProperties, 2, 0, 1)                            \
  V(I32x4Mul, Operator::kCommutative, 2, 0, 1)                             \
  V(I32x4MinS, Operator::kCommutative, 2, 0, 1)                            \
  V(I32x4MaxS, Operator::kCommutative, 2, 0, 1)                            \
  V(I32x4Eq, Operator::kCommutative, 2, 0, 1)                              \
  V(I32x4Ne, Operator::kCommutative, 2, 0, 1)                              \
  V(I32x4GtS, Operator::kNoProperties, 2, 0, 1)                            \
  V(I32x4GeS, Operator::kNoProperties, 2, 0, 1)                            \
  V(I32x4UConvertF32x4, Operator::kNoProperties, 1, 0, 1)                  \
  V(I32x4UConvertI16x8Low, Operator::kNoProperties, 1, 0, 1)               \
  V(I32x4UConvertI16x8High, Operator::kNoProperties, 1, 0, 1)              \
  V(I32x4ShrU, Operator::kNoProperties, 2, 0, 1)                           \
  V(I32x4MinU, Operator::kCommutative, 2, 0, 1)                            \
  V(I32x4MaxU, Operator::kCommutative, 2, 0, 1)                            \
  V(I32x4GtU, Operator::kNoProperties, 2, 0, 1)                            \
  V(I32x4GeU, Operator::kNoProperties, 2, 0, 1)                            \
  V(I32x4Abs, Operator::kNoProperties, 1, 0, 1)                            \
  V(I32x4BitMask, Operator::kNoProperties, 1, 0, 1)                        \
  V(I32x4DotI16x8S, Operator::kCommutative, 2, 0, 1)                       \
  V(I32x4ExtMulLowI16x8S, Operator::kCommutative, 2, 0, 1)                 \
  V(I32x4ExtMulHighI16x8S, Operator::kCommutative, 2, 0, 1)                \
  V(I32x4ExtMulLowI16x8U, Operator::kCommutative, 2, 0, 1)                 \
  V(I32x4ExtMulHighI16x8U, Operator::kCommutative, 2, 0, 1)                \
  V(I32x4ExtAddPairwiseI16x8S, Operator::kNoProperties, 1, 0, 1)           \
  V(I32x4ExtAddPairwiseI16x8U, Operator::kNoProperties, 1, 0, 1)           \
  V(I32x4TruncSatF64x2SZero, Operator::kNoProperties, 1, 0, 1)             \
  V(I32x4TruncSatF64x2UZero, Operator::kNoProperties, 1, 0, 1)             \
  V(I16x8Splat, Operator::kNoProperties, 1, 0, 1)                          \
  V(I16x8SConvertI8x16Low, Operator::kNoProperties, 1, 0, 1)               \
  V(I16x8SConvertI8x16High, Operator::kNoProperties, 1, 0, 1)              \
  V(I16x8Neg, Operator::kNoProperties, 1, 0, 1)                            \
  V(I16x8Shl, Operator::kNoProperties, 2, 0, 1)                            \
  V(I16x8ShrS, Operator::kNoProperties, 2, 0, 1)                           \
  V(I16x8SConvertI32x4, Operator::kNoProperties, 2, 0, 1)                  \
  V(I16x8Add, Operator::kCommutative, 2, 0, 1)                             \
  V(I16x8AddSatS, Operator::kCommutative, 2, 0, 1)                         \
  V(I16x8Sub, Operator::kNoProperties, 2, 0, 1)                            \
  V(I16x8SubSatS, Operator::kNoProperties, 2, 0, 1)                        \
  V(I16x8Mul, Operator::kCommutative, 2, 0, 1)                             \
  V(I16x8MinS, Operator::kCommutative, 2, 0, 1)                            \
  V(I16x8MaxS, Operator::kCommutative, 2, 0, 1)                            \
  V(I16x8Eq, Operator::kCommutative, 2, 0, 1)                              \
  V(I16x8Ne, Operator::kCommutative, 2, 0, 1)                              \
  V(I16x8GtS, Operator::kNoProperties, 2, 0, 1)                            \
  V(I16x8GeS, Operator::kNoProperties, 2, 0, 1)                            \
  V(I16x8UConvertI8x16Low, Operator::kNoProperties, 1, 0, 1)               \
  V(I16x8UConvertI8x16High, Operator::kNoProperties, 1, 0, 1)              \
  V(I16x8ShrU, Operator::kNoProperties, 2, 0, 1)                           \
  V(I16x8UConvertI32x4, Operator::kNoProperties, 2, 0, 1)                  \
  V(I16x8AddSatU, Operator::kCommutative, 2, 0, 1)                         \
  V(I16x8SubSatU, Operator::kNoProperties, 2, 0, 1)                        \
  V(I16x8MinU, Operator::kCommutative, 2, 0, 1)                            \
  V(I16x8MaxU, Operator::kCommutative, 2, 0, 1)                            \
  V(I16x8GtU, Operator::kNoProperties, 2, 0, 1)                            \
  V(I16x8GeU, Operator::kNoProperties, 2, 0, 1)                            \
  V(I16x8RoundingAverageU, Operator::kCommutative, 2, 0, 1)                \
  V(I16x8Q15MulRSatS, Operator::kCommutative, 2, 0, 1)                     \
  V(I16x8Abs, Operator::kNoProperties, 1, 0, 1)                            \
  V(I16x8BitMask, Operator::kNoProperties, 1, 0, 1)                        \
  V(I16x8ExtMulLowI8x16S, Operator::kCommutative, 2, 0, 1)                 \
  V(I16x8ExtMulHighI8x16S, Operator::kCommutative, 2, 0, 1)                \
  V(I16x8ExtMulLowI8x16U, Operator::kCommutative, 2, 0, 1)                 \
  V(I16x8ExtMulHighI8x16U, Operator::kCommutative, 2, 0, 1)                \
  V(I16x8ExtAddPairwiseI8x16S, Operator::kNoProperties, 1, 0, 1)           \
  V(I16x8ExtAddPairwiseI8x16U, Operator::kNoProperties, 1, 0, 1)           \
  V(I8x16Splat, Operator::kNoProperties, 1, 0, 1)                          \
  V(I8x16Neg, Operator::kNoProperties, 1, 0, 1)                            \
  V(I8x16Shl, Operator::kNoProperties, 2, 0, 1)                            \
  V(I8x16ShrS, Operator::kNoProperties, 2, 0, 1)                           \
  V(I8x16SConvertI16x8, Operator::kNoProperties, 2, 0, 1)                  \
  V(I8x16Add, Operator::kCommutative, 2, 0, 1)                             \
  V(I8x16AddSatS, Operator::kCommutative, 2, 0, 1)                         \
  V(I8x16Sub, Operator::kNoProperties, 2, 0, 1)                            \
  V(I8x16SubSatS, Operator::kNoProperties, 2, 0, 1)                        \
  V(I8x16MinS, Operator::kCommutative, 2, 0, 1)                            \
  V(I8x16MaxS, Operator::kCommutative, 2, 0, 1)                            \
  V(I8x16Eq, Operator::kCommutative, 2, 0, 1)                              \
  V(I8x16Ne, Operator::kCommutative, 2, 0, 1)                              \
  V(I8x16GtS, Operator::kNoProperties, 2, 0, 1)                            \
  V(I8x16GeS, Operator::kNoProperties, 2, 0, 1)                            \
  V(I8x16ShrU, Operator::kNoProperties, 2, 0, 1)                           \
  V(I8x16UConvertI16x8, Operator::kNoProperties, 2, 0, 1)                  \
  V(I8x16AddSatU, Operator::kCommutative, 2, 0, 1)                         \
  V(I8x16SubSatU, Operator::kNoProperties, 2, 0, 1)                        \
  V(I8x16MinU, Operator::kCommutative, 2, 0, 1)                            \
  V(I8x16MaxU, Operator::kCommutative, 2, 0, 1)                            \
  V(I8x16GtU, Operator::kNoProperties, 2, 0, 1)                            \
  V(I8x16GeU, Operator::kNoProperties, 2, 0, 1)                            \
  V(I8x16RoundingAverageU, Operator::kCommutative, 2, 0, 1)                \
  V(I8x16Popcnt, Operator::kNoProperties, 1, 0, 1)                         \
  V(I8x16Abs, Operator::kNoProperties, 1, 0, 1)                            \
  V(I8x16BitMask, Operator::kNoProperties, 1, 0, 1)                        \
  V(S128Zero, Operator::kNoProperties, 0, 0, 1)                            \
  V(S128And, Operator::kAssociative | Operator::kCommutative, 2, 0, 1)     \
  V(S128Or, Operator::kAssociative | Operator::kCommutative, 2, 0, 1)      \
  V(S128Xor, Operator::kAssociative | Operator::kCommutative, 2, 0, 1)     \
  V(S128Not, Operator::kNoProperties, 1, 0, 1)                             \
  V(S128Select, Operator::kNoProperties, 3, 0, 1)                          \
  V(S128AndNot, Operator::kNoProperties, 2, 0, 1)                          \
  V(V128AnyTrue, Operator::kNoProperties, 1, 0, 1)                         \
  V(I64x2AllTrue, Operator::kNoProperties, 1, 0, 1)                        \
  V(I32x4AllTrue, Operator::kNoProperties, 1, 0, 1)                        \
  V(I16x8AllTrue, Operator::kNoProperties, 1, 0, 1)                        \
  V(I8x16AllTrue, Operator::kNoProperties, 1, 0, 1)                        \
  V(I8x16Swizzle, Operator::kNoProperties, 2, 0, 1)

#else
// The format is:
// V(Name, properties, value_input_count, control_input_count, output_count)
#define MACHINE_PURE_OP_LIST(V)                                            \
  V(F32x8Splat, Operator::kNoProperties, 1, 0, 1)                          \
  V(F32x8Add, Operator::kCommutative, 2, 0, 1)                             \
  V(F32x8Sub, Operator::kNoProperties, 2, 0, 1)                            \
  V(F32x8Mul, Operator::kCommutative, 2, 0, 1)                             \
  V(F32x8Div, Operator::kNoProperties, 2, 0, 1)                            \
  V(F32x8Min, Operator::kCommutative, 2, 0, 1)                             \
  V(F32x8Max, Operator::kCommutative, 2, 0, 1)                             \
  V(F32x8Eq, Operator::kCommutative, 2, 0, 1)                              \
  V(F32x8Ne, Operator::kCommutative, 2, 0, 1)                              \
  V(F32x8Lt, Operator::kNoProperties, 2, 0, 1)                             \
  V(F32x8Le, Operator::kNoProperties, 2, 0, 1)                             \
  V(S256Select, Operator::kNoProperties, 3, 0, 1)

#endif


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
