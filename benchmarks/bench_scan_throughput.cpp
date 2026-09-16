// Raw scan throughput: SUM() over a resident int64_t column, no predicate,
// no MVCC — this isolates raw scan throughput from transactional overhead
// claim is actually about. See BENCHMARKS.md for how to read GB/s here
// versus this machine's real memory bandwidth ceiling, and for the
// separately-documented reference-hardware projection.
#include <benchmark/benchmark.h>
#include <random>
#include <vector>

#include "coredb/simd/kernels.h"
#include "coredb/util/cpu_features.h"

namespace {

std::vector<int64_t> MakeColumn(size_t n) {
  std::mt19937_64 rng(1234);
  std::uniform_int_distribution<int64_t> dist(-1000, 1000);
  std::vector<int64_t> v(n);
  for (auto& x : v) x = dist(rng);
  return v;
}

}  // namespace

static void BM_ScanThroughput_Dispatched(benchmark::State& state) {
  const size_t n = static_cast<size_t>(state.range(0));
  const std::vector<int64_t> column = MakeColumn(n);
  int64_t sink = 0;
  for (auto _ : state) {
    sink += coredb::simd::Sum(column.data(), n);
    benchmark::DoNotOptimize(sink);
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(n) *
                           static_cast<int64_t>(sizeof(int64_t)));
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(n));
  state.counters["simd_tier"] = static_cast<double>(coredb::util::DetectCapabilities().best_available);
}
// 1K rows (L1-resident) up to 256M rows (~2 GiB, forces main-memory
// bandwidth-bound behavior) so the benchmark output shows where the curve
// stops scaling with cache size.
BENCHMARK(BM_ScanThroughput_Dispatched)
    ->Arg(1 << 10)
    ->Arg(1 << 14)
    ->Arg(1 << 18)
    ->Arg(1 << 20)
    ->Arg(1 << 22)
    ->Arg(1 << 24)
    ->Arg(1 << 26)
    ->Arg(1 << 28)
    ->Unit(benchmark::kMillisecond);

int main(int argc, char** argv) {
  const auto& caps = coredb::util::DetectCapabilities();
  std::fprintf(stderr, "=== CoreDB bench_scan_throughput ===\n%s\n=====================================\n",
               coredb::util::DescribeCapabilities(caps).c_str());
  ::benchmark::Initialize(&argc, argv);
  if (::benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
  ::benchmark::RunSpecifiedBenchmarks();
  ::benchmark::Shutdown();
  return 0;
}
