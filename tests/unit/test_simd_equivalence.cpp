#include "coredb/simd/kernels.h"
#include "coredb/util/cpu_features.h"

#include <random>
#include <vector>

#include <gtest/gtest.h>

using namespace coredb::simd;

namespace {

std::vector<int64_t> RandomColumn(size_t n, int64_t lo, int64_t hi, uint32_t seed) {
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<int64_t> dist(lo, hi);
  std::vector<int64_t> v(n);
  for (auto& x : v) x = dist(rng);
  return v;
}

}  // namespace

// Odd, not-a-multiple-of-any-lane-width sizes on purpose: they force every
// kernel's scalar tail-loop path to actually run.
class SimdEquivalence : public ::testing::TestWithParam<size_t> {};

TEST_P(SimdEquivalence, SumMatchesScalarAcrossAvailableTiers) {
  const size_t n = GetParam();
  const auto data = RandomColumn(n, -1000, 1000, 42);
  const int64_t expected = Sum_Scalar(data.data(), n);

#if defined(__aarch64__) || defined(__ARM_NEON)
  EXPECT_EQ(Sum_Neon(data.data(), n), expected);
#endif
#if defined(__x86_64__) || defined(_M_X64)
  const auto& caps = coredb::util::DetectCapabilities();
  if (caps.avx2_available) EXPECT_EQ(Sum_Avx2(data.data(), n), expected);
  if (caps.avx512_available) EXPECT_EQ(Sum_Avx512(data.data(), n), expected);
#endif
  EXPECT_EQ(Sum(data.data(), n), expected) << "runtime dispatch must agree with scalar too";
}

TEST_P(SimdEquivalence, FilterGtSumMatchesScalarAcrossAvailableTiers) {
  const size_t n = GetParam();
  const auto pred = RandomColumn(n, 0, 100, 7);
  const auto out = RandomColumn(n, -50, 50, 99);
  const int64_t threshold = 50;

  const FilterSumResult expected = FilterGtSum_Scalar(pred.data(), out.data(), n, threshold);

#if defined(__aarch64__) || defined(__ARM_NEON)
  {
    const auto got = FilterGtSum_Neon(pred.data(), out.data(), n, threshold);
    EXPECT_EQ(got.matched_count, expected.matched_count);
    EXPECT_EQ(got.sum, expected.sum);
  }
#endif
#if defined(__x86_64__) || defined(_M_X64)
  const auto& caps = coredb::util::DetectCapabilities();
  if (caps.avx2_available) {
    const auto got = FilterGtSum_Avx2(pred.data(), out.data(), n, threshold);
    EXPECT_EQ(got.matched_count, expected.matched_count);
    EXPECT_EQ(got.sum, expected.sum);
  }
  if (caps.avx512_available) {
    const auto got = FilterGtSum_Avx512(pred.data(), out.data(), n, threshold);
    EXPECT_EQ(got.matched_count, expected.matched_count);
    EXPECT_EQ(got.sum, expected.sum);
  }
#endif
  const auto dispatched = FilterGtSum(pred.data(), out.data(), n, threshold);
  EXPECT_EQ(dispatched.matched_count, expected.matched_count);
  EXPECT_EQ(dispatched.sum, expected.sum);
}

INSTANTIATE_TEST_SUITE_P(VariousSizes, SimdEquivalence,
                          ::testing::Values(size_t{0}, size_t{1}, size_t{2}, size_t{3}, size_t{7}, size_t{8},
                                             size_t{15}, size_t{16}, size_t{17}, size_t{1000}, size_t{10007}));

TEST(SimdEquivalence, ReportsDetectedCapabilities) {
  const auto& caps = coredb::util::DetectCapabilities();
  EXPECT_FALSE(caps.architecture.empty());
  EXPECT_GT(caps.logical_cores, 0u);
  fprintf(stderr, "%s\n", coredb::util::DescribeCapabilities(caps).c_str());
}
