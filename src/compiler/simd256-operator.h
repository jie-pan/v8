
#ifndef V8_COMPILER_SIMD256_OPERATOR_H_
#define V8_COMPILER_SIMD256_OPERATOR_H_

#include "src/compiler/common-operator.h"
#include "src/compiler/node-properties.h"
#include "src/compiler/operator.h"


namespace v8 {
class CFunctionInfo;

namespace internal {

// Forward declarations.
enum class AbortReason : uint8_t;
class Zone;

namespace compiler {

// Forward declarations.
class Operator;
struct Simd256OperatorGlobalCache;

class V8_EXPORT_PRIVATE Simd256OperatorBuilder final
    : public NON_EXPORTED_BASE(ZoneObject) {
 public:
  explicit Simd256OperatorBuilder(Zone* zone);
  Simd256OperatorBuilder(const Simd256OperatorBuilder&) = delete;
  Simd256OperatorBuilder& operator=(const Simd256OperatorBuilder&) =
      delete;

  //const Operator* BooleanNot();

  const Operator* F32x8Splat();
  const Operator* F32x8ExtractLane(int32_t);
  const Operator* F32x8ReplaceLane(int32_t);
  const Operator* F32x8SConvertI32x4();
  const Operator* F32x8UConvertI32x4();
  const Operator* F32x8Abs();
  const Operator* F32x8Neg();
  const Operator* F32x8Sqrt();
  const Operator* F32x8RecipApprox();
  const Operator* F32x8RecipSqrtApprox();
  const Operator* F32x8Add();
  const Operator* F32x8Sub();
  const Operator* F32x8Mul();
  const Operator* F32x8Div();
  const Operator* F32x8Min();
  const Operator* F32x8Max();
  const Operator* F32x8Eq();
  const Operator* F32x8Ne();
  const Operator* F32x8Lt();
  const Operator* F32x8Le();
  const Operator* F32x8Qfma();
  const Operator* F32x8Qfms();
  const Operator* F32x8Pmin();
  const Operator* F32x8Pmax();
  const Operator* F32x8Ceil();
  const Operator* F32x8Floor();
  const Operator* F32x8Trunc();
  const Operator* F32x8NearestInt();
  const Operator* F32x8DemoteF64x2Zero();


 private:
  Zone* zone() const { return zone_; }

  const Simd256OperatorGlobalCache& cache_;
  Zone* const zone_;
};


}  // namespace compiler
}  // namespace internal
}
#endif
