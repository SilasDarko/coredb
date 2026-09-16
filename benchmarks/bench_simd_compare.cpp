// Head-to-head comparison of the scalar kernel against every SIMD tier this
// machine can actually execute. On arm64 that's scalar vs. NEON; the
// AVX2/AVX-512 kernels compile everywhere (as real, tested code — see
// tests/unit/test_simd_equivalence.cpp) but are only ever benchmarked on an
// x86_64 host where the CPU actually supports them.
#include <benchmark/benchmark.h>
#include <random>
#include <vector>

#include "coredb/simd/kernels.h"
#include "coredb/util/cpu_features.h"

namespace {
std::vector<int64_t> MakeColumn(size_t n) {
  std::mt19937_64 rng(99);
  std::uniform_int_distribution<int64_t> dist(0, 1000);
  std::vector<int64_t> v(n);
  for (auto& x : v) x = dist(rng);
  return v;
}
constexpr size_t kN = 1 << 24;  // 16M rows == 128 MiB, well past L2/L3
}  // namespace

static void BM_Sum_Scalar(benchmark::State& state) {
  const auto data = MakeColumn(kN);
  int64_t sink = 0;
  for (auto _ : state) {
    sink += coredb::simd::Sum_Scalar(data.data(), kN);
    benchmark::DoNotOptimize(sink);
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * kN * sizeof(int64_t)));
}
BENCHMARK(BM_Sum_Scalar)->Unit(benchmark::kMillisecond);

#if defined(__aarch64__) || defined(__ARM_NEON)
static void BM_Sum_Neon(benchmark::State& state) {
  const auto data = MakeColumn(kN);
  int64_t sink = 0;
  for (auto _ : state) {
    sink += coredb::simd::Sum_Neon(data.data(), kN);
    benchmark::DoNotOptimize(sink);
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * kN * sizeof(int64_t)));
}
BENCHMARK(BM_Sum_Neon)->Unit(benchmark::kMillisecond);
#endif

#if defined(__x86_64__) || defined(_M_X64)
static void BM_Sum_Avx2(benchmark::State& state) {
  if (!coredb::util::DetectCapabilities().avx2_available) {
    state.SkipWithError("AVX2 not available on this CPU");
    return;
  }
  const auto data = MakeColumn(kN);
  int64_t sink = 0;
  for (auto _ : state) {
    sink += coredb::simd::Sum_Avx2(data.data(), kN);
    benchmark::DoNotOptimize(sink);
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * kN * sizeof(int64_t)));
}
BENCHMARK(BM_Sum_Avx2)->Unit(benchmark::kMillisecond);

static void BM_Sum_Avx512(benchmark::State& state) {
  if (!coredb::util::DetectCapabilities().avx512_available) {
    state.SkipWithError("AVX-512 not available on this CPU");
    return;
  }
  const auto data = MakeColumn(kN);
  int64_t sink = 0;
  for (auto _ : state) {
    sink += coredb::simd::Sum_Avx512(data.data(), kN);
    benchmark::DoNotOptimize(sink);
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * kN * sizeof(int64_t)));
}
BENCHMARK(BM_Sum_Avx512)->Unit(benchmark::kMillisecond);
#endif

static void BM_FilterGtSum_Scalar(benchmark::State& state) {
  const auto pred = MakeColumn(kN);
  const auto out = MakeColumn(kN);
  coredb::simd::FilterSumResult sink;
  for (auto _ : state) {
    sink = coredb::simd::FilterGtSum_Scalar(pred.data(), out.data(), kN, 500);
    benchmark::DoNotOptimize(sink);
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * kN * 2 * sizeof(int64_t)));
}
BENCHMARK(BM_FilterGtSum_Scalar)->Unit(benchmark::kMillisecond);

#if defined(__aarch64__) || defined(__ARM_NEON)
static void BM_FilterGtSum_Neon(benchmark::State& state) {
  const auto pred = MakeColumn(kN);
  const auto out = MakeColumn(kN);
  coredb::simd::FilterSumResult sink;
  for (auto _ : state) {
    sink = coredb::simd::FilterGtSum_Neon(pred.data(), out.data(), kN, 500);
    benchmark::DoNotOptimize(sink);
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * kN * 2 * sizeof(int64_t)));
}
BENCHMARK(BM_FilterGtSum_Neon)->Unit(benchmark::kMillisecond);
#endif

int main(int argc, char** argv) {
  const auto& caps = coredb::util::DetectCapabilities();
  std::fprintf(stderr, "=== CoreDB bench_simd_compare ===\n%s\n==================================\n",
               coredb::util::DescribeCapabilities(caps).c_str());
  ::benchmark::Initialize(&argc, argv);
  if (::benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
  ::benchmark::RunSpecifiedBenchmarks();
  ::benchmark::Shutdown();
  return 0;
}
