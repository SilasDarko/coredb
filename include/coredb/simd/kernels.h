#pragma once

#include <cstddef>
#include <cstdint>

// CoreDB's SIMD kernels operate on dense int64_t arrays (a segment's raw
// column storage) and implement one query shape each:
//   Sum          — unconditional column sum (pure scan-throughput kernel)
//   FilterGtSum  — SUM(out) WHERE pred > threshold, branchless (used by the
//                  Volcano vs. vectorized vs. JIT execution comparison)
//
// Every tier (scalar always; NEON on arm64; AVX2/AVX-512 on x86_64) is
// numerically identical — see tests/unit/test_simd_equivalence.cpp — they
// only differ in how many int64 lanes they process per instruction.
namespace coredb::simd {

struct FilterSumResult {
  uint64_t matched_count = 0;
  int64_t sum = 0;
};

// Always available.
int64_t Sum_Scalar(const int64_t* data, size_t n);
FilterSumResult FilterGtSum_Scalar(const int64_t* pred, const int64_t* out, size_t n, int64_t threshold);

#if defined(__aarch64__) || defined(__ARM_NEON)
int64_t Sum_Neon(const int64_t* data, size_t n);
FilterSumResult FilterGtSum_Neon(const int64_t* pred, const int64_t* out, size_t n, int64_t threshold);
#endif

#if defined(__x86_64__) || defined(_M_X64)
int64_t Sum_Avx2(const int64_t* data, size_t n);
FilterSumResult FilterGtSum_Avx2(const int64_t* pred, const int64_t* out, size_t n, int64_t threshold);

int64_t Sum_Avx512(const int64_t* data, size_t n);
FilterSumResult FilterGtSum_Avx512(const int64_t* pred, const int64_t* out, size_t n, int64_t threshold);
#endif

// Dispatches to the best kernel tier detected at process startup
// (util::DetectCapabilities()). This is what benchmarks and the vectorized
// execution operator call; use the tier-specific functions above only when
// you need a *specific* tier (equivalence tests, SIMD-comparison
// benchmarks).
int64_t Sum(const int64_t* data, size_t n);
FilterSumResult FilterGtSum(const int64_t* pred, const int64_t* out, size_t n, int64_t threshold);

}  // namespace coredb::simd
