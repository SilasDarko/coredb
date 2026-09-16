// Volcano iterator vs. vectorized-batch vs. LLVM-JIT-fused execution of the
// same SUM(agg) WHERE pred > threshold query over identical segments. This
// is the benchmark the "Nx speedup over Volcano" style claim must be read
// from — see BENCHMARKS.md for the measured ratio on this machine.
#include <benchmark/benchmark.h>
#include <random>
#include <vector>

#include "coredb/exec/query.h"
#include "coredb/util/cpu_features.h"

namespace {

using coredb::storage::ColumnType;
using coredb::storage::Row;
using coredb::storage::Schema;
using coredb::storage::Segment;

Schema WideSchema() { return {{"pred", ColumnType::kInt64}, {"agg", ColumnType::kInt64}}; }

std::vector<Segment> MakeSegments(size_t total_rows, size_t rows_per_segment) {
  std::mt19937_64 rng(777);
  std::uniform_int_distribution<int64_t> pred_dist(0, 1000);
  std::uniform_int_distribution<int64_t> agg_dist(-500, 500);

  std::vector<Segment> segments;
  uint64_t segment_id = 1;
  for (size_t offset = 0; offset < total_rows; offset += rows_per_segment) {
    const size_t n = std::min(rows_per_segment, total_rows - offset);
    std::vector<Row> rows(n);
    for (auto& r : rows) r = {pred_dist(rng), agg_dist(rng)};
    segments.push_back(Segment::Build(segment_id++, WideSchema(), rows));
  }
  return segments;
}

std::vector<const Segment*> Ptrs(const std::vector<Segment>& segments) {
  std::vector<const Segment*> ptrs;
  ptrs.reserve(segments.size());
  for (const auto& s : segments) ptrs.push_back(&s);
  return ptrs;
}

constexpr size_t kTotalRows = 4'000'000;
constexpr size_t kRowsPerSegment = 65536;

}  // namespace

static void BM_Exec_Volcano(benchmark::State& state) {
  const auto segments = MakeSegments(kTotalRows, kRowsPerSegment);
  const auto ptrs = Ptrs(segments);
  coredb::exec::AggregateQuery query{0, 1, 500};
  coredb::exec::AggregateResult sink;
  for (auto _ : state) {
    sink = coredb::exec::RunVolcano(ptrs, query);
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * kTotalRows));
}
BENCHMARK(BM_Exec_Volcano)->Unit(benchmark::kMillisecond);

static void BM_Exec_Vectorized(benchmark::State& state) {
  const auto segments = MakeSegments(kTotalRows, kRowsPerSegment);
  const auto ptrs = Ptrs(segments);
  coredb::exec::AggregateQuery query{0, 1, 500};
  coredb::exec::AggregateResult sink;
  for (auto _ : state) {
    sink = coredb::exec::RunVectorized(ptrs, query);
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * kTotalRows));
}
BENCHMARK(BM_Exec_Vectorized)->Unit(benchmark::kMillisecond);

#if defined(COREDB_ENABLE_JIT)
static void BM_Exec_Jit(benchmark::State& state) {
  const auto segments = MakeSegments(kTotalRows, kRowsPerSegment);
  const auto ptrs = Ptrs(segments);
  coredb::exec::AggregateQuery query{0, 1, 500};
  coredb::jit::QueryJit compiled;  // compiled once, outside the timed loop
  coredb::exec::AggregateResult sink;
  for (auto _ : state) {
    sink = coredb::exec::RunJit(ptrs, query, compiled);
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * kTotalRows));
}
BENCHMARK(BM_Exec_Jit)->Unit(benchmark::kMillisecond);
#endif

int main(int argc, char** argv) {
  const auto& caps = coredb::util::DetectCapabilities();
  std::fprintf(stderr, "=== CoreDB bench_exec_compare ===\n%s\nrows=%zu, rows/segment=%zu\n"
                        "==================================\n",
               coredb::util::DescribeCapabilities(caps).c_str(), kTotalRows, kRowsPerSegment);
  ::benchmark::Initialize(&argc, argv);
  if (::benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
  ::benchmark::RunSpecifiedBenchmarks();
  ::benchmark::Shutdown();
  return 0;
}
