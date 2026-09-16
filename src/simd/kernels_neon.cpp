#include "coredb/simd/kernels.h"

#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>

namespace coredb::simd {

int64_t Sum_Neon(const int64_t* data, size_t n) {
  // Two independent accumulator chains (4 lanes/iteration total) rather
  // than one: a single int64x2_t accumulator serializes on
  // acc = acc + next, and that dependency chain — not load/ALU
  // throughput — is what actually bottlenecks this loop. Two chains let
  // the CPU overlap their additions instead of waiting on each other; see
  // BENCHMARKS.md for the measured difference this made.
  int64x2_t acc0 = vdupq_n_s64(0);
  int64x2_t acc1 = vdupq_n_s64(0);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    acc0 = vaddq_s64(acc0, vld1q_s64(data + i));
    acc1 = vaddq_s64(acc1, vld1q_s64(data + i + 2));
  }
  int64_t sum = vaddvq_s64(vaddq_s64(acc0, acc1));
  for (; i < n; ++i) sum += data[i];  // tail: n not a multiple of the 4-lane width
  return sum;
}

FilterSumResult FilterGtSum_Neon(const int64_t* pred, const int64_t* out, size_t n, int64_t threshold) {
  const int64x2_t threshold_vec = vdupq_n_s64(threshold);
  int64x2_t sum_acc = vdupq_n_s64(0);
  int64x2_t count_acc = vdupq_n_s64(0);

  size_t i = 0;
  for (; i + 2 <= n; i += 2) {
    const int64x2_t pred_vec = vld1q_s64(pred + i);
    const int64x2_t out_vec = vld1q_s64(out + i);
    // mask lanes are all-ones (-1) where pred > threshold, else all-zero —
    // branchless predication: AND-in the value, subtract to count.
    const int64x2_t mask = vreinterpretq_s64_u64(vcgtq_s64(pred_vec, threshold_vec));
    sum_acc = vaddq_s64(sum_acc, vandq_s64(mask, out_vec));
    count_acc = vsubq_s64(count_acc, mask);
  }

  FilterSumResult result;
  result.sum = vaddvq_s64(sum_acc);
  result.matched_count = static_cast<uint64_t>(vaddvq_s64(count_acc));
  for (; i < n; ++i) {
    if (pred[i] > threshold) {
      result.sum += out[i];
      result.matched_count += 1;
    }
  }
  return result;
}

}  // namespace coredb::simd

#endif  // __aarch64__ || __ARM_NEON
