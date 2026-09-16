#include "coredb/simd/kernels.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>

namespace coredb::simd {

__attribute__((target("avx512f,avx512bw,avx512dq"))) int64_t Sum_Avx512(const int64_t* data, size_t n) {
  __m512i acc = _mm512_setzero_si512();
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    acc = _mm512_add_epi64(acc, _mm512_loadu_si512(reinterpret_cast<const void*>(data + i)));
  }
  int64_t sum = _mm512_reduce_add_epi64(acc);
  for (; i < n; ++i) sum += data[i];
  return sum;
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) FilterSumResult FilterGtSum_Avx512(const int64_t* pred,
                                                                                          const int64_t* out,
                                                                                          size_t n,
                                                                                          int64_t threshold) {
  const __m512i threshold_vec = _mm512_set1_epi64(threshold);
  __m512i sum_acc = _mm512_setzero_si512();
  uint64_t matched = 0;

  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    const __m512i pred_vec = _mm512_loadu_si512(reinterpret_cast<const void*>(pred + i));
    const __m512i out_vec = _mm512_loadu_si512(reinterpret_cast<const void*>(out + i));
    const __mmask8 mask = _mm512_cmpgt_epi64_mask(pred_vec, threshold_vec);
    sum_acc = _mm512_add_epi64(sum_acc, _mm512_maskz_mov_epi64(mask, out_vec));
    matched += static_cast<unsigned>(__builtin_popcount(mask));
  }

  FilterSumResult result;
  result.sum = _mm512_reduce_add_epi64(sum_acc);
  result.matched_count = matched;
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
