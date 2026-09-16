#include "coredb/jit/query_jit.h"
#include "coredb/simd/kernels.h"

#include <random>
#include <vector>

#include <gtest/gtest.h>

using namespace coredb;

namespace {
std::vector<int64_t> RandomColumn(size_t n, int64_t lo, int64_t hi, uint32_t seed) {
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<int64_t> dist(lo, hi);
  std::vector<int64_t> v(n);
  for (auto& x : v) x = dist(rng);
  return v;
}
}  // namespace

class JitEquivalence : public ::testing::TestWithParam<size_t> {};

TEST_P(JitEquivalence, MatchesScalarKernelExactly) {
  const size_t n = GetParam();
  const auto pred = RandomColumn(n, 0, 200, 11);
  const auto out = RandomColumn(n, -500, 500, 22);
  const int64_t threshold = 100;

  const auto expected = simd::FilterGtSum_Scalar(pred.data(), out.data(), n, threshold);

  jit::QueryJit query_jit;
  const auto got = query_jit.Run(pred.data(), out.data(), n, threshold);

  EXPECT_EQ(got.matched_count, expected.matched_count);
  EXPECT_EQ(got.sum, expected.sum);
}

INSTANTIATE_TEST_SUITE_P(VariousSizes, JitEquivalence,
                          ::testing::Values(size_t{0}, size_t{1}, size_t{2}, size_t{100}, size_t{10007}));

TEST(JitEquivalence, CompiledFunctionIsReusedAcrossManyCalls) {
  jit::QueryJit query_jit;
  const auto pred = RandomColumn(5000, 0, 100, 5);
  const auto out = RandomColumn(5000, 0, 10, 6);
  const auto first = query_jit.Run(pred.data(), out.data(), pred.size(), 50);
  for (int i = 0; i < 10; ++i) {
    const auto again = query_jit.Run(pred.data(), out.data(), pred.size(), 50);
    EXPECT_EQ(again.sum, first.sum);
    EXPECT_EQ(again.matched_count, first.matched_count);
  }
}
