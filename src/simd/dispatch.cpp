#include "coredb/simd/kernels.h"
#include "coredb/util/cpu_features.h"

namespace coredb::simd {

int64_t Sum(const int64_t* data, size_t n) {
  const util::SimdLevel level = util::DetectCapabilities().best_available;
#if defined(__x86_64__) || defined(_M_X64)
  if (level == util::SimdLevel::kAvx512) return Sum_Avx512(data, n);
  if (level == util::SimdLevel::kAvx2) return Sum_Avx2(data, n);
#elif defined(__aarch64__) || defined(__ARM_NEON)
  if (level == util::SimdLevel::kNeon) return Sum_Neon(data, n);
#endif
  (void)level;
  return Sum_Scalar(data, n);
}

FilterSumResult FilterGtSum(const int64_t* pred, const int64_t* out, size_t n, int64_t threshold) {
  const util::SimdLevel level = util::DetectCapabilities().best_available;
#if defined(__x86_64__) || defined(_M_X64)
  if (level == util::SimdLevel::kAvx512) return FilterGtSum_Avx512(pred, out, n, threshold);
  if (level == util::SimdLevel::kAvx2) return FilterGtSum_Avx2(pred, out, n, threshold);
#elif defined(__aarch64__) || defined(__ARM_NEON)
  if (level == util::SimdLevel::kNeon) return FilterGtSum_Neon(pred, out, n, threshold);
#endif
  (void)level;
  return FilterGtSum_Scalar(pred, out, n, threshold);
}

}  // namespace coredb::simd
