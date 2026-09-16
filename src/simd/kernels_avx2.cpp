#include "coredb/simd/kernels.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>

namespace coredb::simd {

namespace {

__attribute__((target("avx2"))) int64_t HorizontalSum(__m256i v) {
  const __m128i lo = _mm256_castsi256_si128(v);
  const __m128i hi = _mm256_extracti128_si256(v, 1);
  const __m128i sum128 = _mm_add_epi64(lo, hi);
  return _mm_cvtsi128_si64(sum128) + _mm_extract_epi64(sum128, 1);
}

}  // namespace

__attribute__((target("avx2"))) int64_t Sum_Avx2(const int64_t* data, size_t n) {
  __m256i acc = _mm256_setzero_si256();
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    acc = _mm256_add_epi64(acc, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i)));
  }
  int64_t sum = HorizontalSum(acc);
  for (; i < n; ++i) sum += data[i];
  return sum;
}

__attribute__((target("avx2"))) FilterSumResult FilterGtSum_Avx2(const int64_t* pred, const int64_t* out,
                                                                   size_t n, int64_t threshold) {
  const __m256i threshold_vec = _mm256_set1_epi64x(threshold);
  __m256i sum_acc = _mm256_setzero_si256();
  __m256i count_acc = _mm256_setzero_si256();

  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    const __m256i pred_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(pred + i));
    const __m256i out_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(out + i));
    const __m256i mask = _mm256_cmpgt_epi64(pred_vec, threshold_vec);  // -1 where pred > threshold, else 0
    sum_acc = _mm256_add_epi64(sum_acc, _mm256_and_si256(mask, out_vec));
    count_acc = _mm256_sub_epi64(count_acc, mask);
  }

  FilterSumResult result;
  result.sum = HorizontalSum(sum_acc);
  result.matched_count = static_cast<uint64_t>(HorizontalSum(count_acc));
  for (; i < n; ++i) {
    if (pred[i] > threshold) {
      result.sum += out[i];
      result.matched_count += 1;
    }
  }
  return result;
}

}  // namespace coredb::simd

#endif  // __x86_64__ || _M_X64
