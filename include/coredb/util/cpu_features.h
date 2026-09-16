#pragma once

#include <cstdint>
#include <string>

namespace coredb::util {

// The SIMD kernel tiers CoreDB ships. Ordered from weakest to strongest so
// callers can do `if (level >= SimdLevel::kAvx2)`.
enum class SimdLevel : int {
  kScalar = 0,
  kNeon = 1,   // arm64 only
  kAvx2 = 2,   // x86_64 only
  kAvx512 = 3, // x86_64 only, requires AVX-512F (+BW/DQ for the kernels we use)
};

const char* ToString(SimdLevel level);

struct CpuCapabilities {
  std::string architecture; // "arm64" or "x86_64" (from uname)
  unsigned logical_cores = 0;
  uint64_t total_memory_bytes = 0;
  // Best SIMD tier CoreDB can actually execute on this machine, decided at
  // process startup by probing the CPU (x86) or the compile target (arm64).
  SimdLevel best_available = SimdLevel::kScalar;
  bool neon_available = false;
  bool avx2_available = false;
  bool avx512_available = false;
};

// Probes the running CPU exactly once (the result is cached in a function
// local static) and returns the capability snapshot. Safe to call from
// benchmarks, tests, and the SIMD dispatcher alike.
const CpuCapabilities& DetectCapabilities();

// Human-readable multi-line summary, e.g. for benchmark headers:
//   architecture: arm64
//   logical_cores: 8
//   memory: 16.0 GiB
//   simd: NEON (AVX2/AVX-512 unavailable: not an x86_64 target)
std::string DescribeCapabilities(const CpuCapabilities& caps);

}  // namespace coredb::util
