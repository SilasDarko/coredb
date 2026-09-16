#include "coredb/util/cpu_features.h"

#include <sstream>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

#if defined(__linux__)
#include <unistd.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <cpuid.h>
#endif

namespace coredb::util {

namespace {

unsigned DetectLogicalCores() {
#if defined(__APPLE__)
  int32_t count = 0;
  size_t size = sizeof(count);
  if (sysctlbyname("hw.logicalcpu", &count, &size, nullptr, 0) == 0 && count > 0) {
    return static_cast<unsigned>(count);
  }
  return 1;
#elif defined(__linux__)
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? static_cast<unsigned>(n) : 1u;
#else
  return 1;
#endif
}

uint64_t DetectTotalMemoryBytes() {
#if defined(__APPLE__)
  uint64_t bytes = 0;
  size_t size = sizeof(bytes);
  if (sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0) {
    return bytes;
  }
  return 0;
#elif defined(__linux__)
  long pages = sysconf(_SC_PHYS_PAGES);
  long page_size = sysconf(_SC_PAGE_SIZE);
  if (pages > 0 && page_size > 0) {
    return static_cast<uint64_t>(pages) * static_cast<uint64_t>(page_size);
  }
  return 0;
#else
  return 0;
#endif
}

std::string DetectArchitectureString() {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#else
  return "unknown";
#endif
}

// x86_64 feature bits are only meaningful (and only compile) on x86_64:
// NEON on arm64 is architecturally mandatory, so there is nothing to probe
// there beyond "were we compiled for arm64".
#if defined(__x86_64__) || defined(_M_X64)
bool ProbeAvx2() { return __builtin_cpu_supports("avx2"); }
bool ProbeAvx512() {
  // AVX-512F (foundation) + BW (byte/word ops) + DQ (doubleword/quadword)
  // cover the kernels CoreDB actually emits.
  return __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") &&
         __builtin_cpu_supports("avx512dq");
}
#endif

CpuCapabilities Probe() {
  CpuCapabilities caps;
  caps.architecture = DetectArchitectureString();
  caps.logical_cores = DetectLogicalCores();
  caps.total_memory_bytes = DetectTotalMemoryBytes();

#if defined(__aarch64__) || defined(_M_ARM64)
  caps.neon_available = true;
  caps.best_available = SimdLevel::kNeon;
#elif defined(__x86_64__) || defined(_M_X64)
  __builtin_cpu_init();
  caps.avx2_available = ProbeAvx2();
  caps.avx512_available = ProbeAvx512();
  if (caps.avx512_available) {
    caps.best_available = SimdLevel::kAvx512;
  } else if (caps.avx2_available) {
    caps.best_available = SimdLevel::kAvx2;
  } else {
    caps.best_available = SimdLevel::kScalar;
  }
#else
  caps.best_available = SimdLevel::kScalar;
#endif
  return caps;
}

}  // namespace

const char* ToString(SimdLevel level) {
  switch (level) {
    case SimdLevel::kScalar: return "scalar";
    case SimdLevel::kNeon: return "neon";
    case SimdLevel::kAvx2: return "avx2";
    case SimdLevel::kAvx512: return "avx512";
  }
  return "unknown";
}

const CpuCapabilities& DetectCapabilities() {
  static const CpuCapabilities kCaps = Probe();
  return kCaps;
}

std::string DescribeCapabilities(const CpuCapabilities& caps) {
  std::ostringstream out;
  out << "architecture: " << caps.architecture << "\n";
  out << "logical_cores: " << caps.logical_cores << "\n";
  out << "memory: " << (caps.total_memory_bytes / (1024.0 * 1024.0 * 1024.0)) << " GiB\n";
  out << "simd: " << ToString(caps.best_available);
  if (caps.architecture == "arm64") {
    out << " (AVX2/AVX-512 unavailable: not an x86_64 target)";
  } else if (caps.architecture == "x86_64") {
    out << " (avx2=" << (caps.avx2_available ? "yes" : "no")
        << ", avx512=" << (caps.avx512_available ? "yes" : "no") << ")";
  }
  return out.str();
}

}  // namespace coredb::util
