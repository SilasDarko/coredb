#include "coredb/exec/query.h"

#include <random>

#include <gtest/gtest.h>

using namespace coredb;
using namespace coredb::storage;

namespace {

Schema Wide() { return {{"pred", ColumnType::kInt64}, {"agg", ColumnType::kInt64}}; }

Segment MakeSegment(uint64_t id, size_t num_rows, int64_t pred_lo, int64_t pred_hi, uint32_t seed) {
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<int64_t> pred_dist(pred_lo, pred_hi);
  std::uniform_int_distribution<int64_t> agg_dist(-100, 100);
  std::vector<Row> rows(num_rows);
  for (auto& r : rows) r = {pred_dist(rng), agg_dist(rng)};
  return Segment::Build(id, Wide(), rows);
}

}  // namespace

TEST(ExecEquivalence, VolcanoAndVectorizedAgree) {
  Segment s1 = MakeSegment(1, 5000, 0, 100, 1);
  Segment s2 = MakeSegment(2, 3000, 0, 100, 2);
  std::vector<const Segment*> segments = {&s1, &s2};

  exec::AggregateQuery q{0, 1, 50};
  const auto volcano = exec::RunVolcano(segments, q);
  const auto vectorized = exec::RunVectorized(segments, q);

  EXPECT_EQ(volcano.matched_count, vectorized.matched_count);
  EXPECT_EQ(volcano.sum, vectorized.sum);
  EXPECT_GT(volcano.matched_count, 0u);
}

TEST(ExecEquivalence, SegmentPruningSkipsWholeSegmentsInBothStrategies) {
  Segment low = MakeSegment(1, 1000, 0, 10, 3);      // entirely below threshold
  Segment high = MakeSegment(2, 1000, 200, 300, 4);  // entirely above threshold
  std::vector<const Segment*> segments = {&low, &high};

  exec::AggregateQuery q{0, 1, 150};
  const auto volcano = exec::RunVolcano(segments, q);
  const auto vectorized = exec::RunVectorized(segments, q);

  EXPECT_EQ(volcano.matched_count, 1000u);  // only `high` contributes
  EXPECT_EQ(volcano.matched_count, vectorized.matched_count);
  EXPECT_EQ(volcano.sum, vectorized.sum);
}

TEST(ExecEquivalence, EmptySegmentListYieldsZero) {
  std::vector<const Segment*> segments;
  exec::AggregateQuery q{0, 1, 0};
  EXPECT_EQ(exec::RunVolcano(segments, q).matched_count, 0u);
  EXPECT_EQ(exec::RunVectorized(segments, q).matched_count, 0u);
}

#if defined(COREDB_ENABLE_JIT)
TEST(ExecEquivalence, JitAgreesWithVolcanoAndVectorized) {
  Segment s1 = MakeSegment(1, 4096, 0, 100, 5);
  Segment s2 = MakeSegment(2, 777, 0, 100, 6);
  std::vector<const Segment*> segments = {&s1, &s2};

  exec::AggregateQuery q{0, 1, 42};
  jit::QueryJit compiled;

  const auto volcano = exec::RunVolcano(segments, q);
  const auto jit_result = exec::RunJit(segments, q, compiled);

  EXPECT_EQ(volcano.matched_count, jit_result.matched_count);
  EXPECT_EQ(volcano.sum, jit_result.sum);
}
#endif
