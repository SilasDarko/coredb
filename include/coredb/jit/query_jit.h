#pragma once

#include <memory>

#include "coredb/simd/kernels.h"

namespace llvm::orc {
class LLJIT;
}

namespace coredb::jit {

// Compiles CoreDB's one supported query shape — SUM(out) WHERE pred >
// threshold — into a native function via LLVM's ORC JIT, and fuses the
// filter and the aggregate into a single generated loop (no interpreter
// dispatch per row, no materializing an intermediate filtered batch). The
// IR is built once in the constructor; every Run() call afterward invokes
// the already-JIT-compiled machine code directly.
class QueryJit {
 public:
  QueryJit();
  ~QueryJit();

  QueryJit(const QueryJit&) = delete;
  QueryJit& operator=(const QueryJit&) = delete;

  simd::FilterSumResult Run(const int64_t* pred, const int64_t* out, size_t n, int64_t threshold) const;

 private:
  using FusedFn = void (*)(const int64_t*, const int64_t*, uint64_t, int64_t, uint64_t*, int64_t*);

  void Compile();

  std::unique_ptr<llvm::orc::LLJIT> jit_;
  FusedFn compiled_fn_ = nullptr;
};

}  // namespace coredb::jit
