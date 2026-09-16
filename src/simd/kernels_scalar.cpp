#include "coredb/simd/kernels.h"

namespace coredb::simd {

int64_t Sum_Scalar(const int64_t* data, size_t n) {
  int64_t sum = 0;
  for (size_t i = 0; i < n; ++i) sum += data[i];
  return sum;
}

FilterSumResult FilterGtSum_Scalar(const int64_t* pred, const int64_t* out, size_t n, int64_t threshold) {
  FilterSumResult result;
  for (size_t i = 0; i < n; ++i) {
    if (pred[i] > threshold) {
      result.sum += out[i];
      result.matched_count += 1;
    }
  }
  return result;
}

}  // namespace coredb::simd
