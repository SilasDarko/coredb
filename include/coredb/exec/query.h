#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "coredb/simd/kernels.h"
#include "coredb/storage/segment.h"

// CoreDB's execution engine deliberately supports one query shape —
// SUM(aggregate_column) WHERE predicate_column > threshold, over int64
// columns — implemented three ways (Volcano iterator, vectorized batches,
// LLVM JIT fusion) so the three strategies can be benchmarked head-to-head
// on identical work. See ARCHITECTURE.md for why the comparison is scoped
// this narrowly rather than building a general expression evaluator.
namespace coredb::exec {

struct AggregateQuery {
  size_t predicate_column;
  size_t aggregate_column;
  int64_t threshold;  // predicate: column > threshold
};

using AggregateResult = simd::FilterSumResult;

// Row-at-a-time iterator model: a chain of virtual Next() calls per row,
// exactly as textbook Volcano/iterator-model execution works. This is the
// deliberately "slow" baseline the other two strategies are compared
// against — see BENCHMARKS.md for measured overhead.
AggregateResult RunVolcano(const std::vector<const storage::Segment*>& segments, const AggregateQuery& query);

// Batches each segment's matching columns into fixed-size chunks and calls
// the SIMD-dispatched kernel per batch — "vectorized batches" per
// ARCHITECTURE.md, with segment-level pruning applied first via
// Segment::CanSkip.
constexpr size_t kDefaultBatchSize = 2048;
AggregateResult RunVectorized(const std::vector<const storage::Segment*>& segments, const AggregateQuery& query,
                               size_t batch_size = kDefaultBatchSize);

}  // namespace coredb::exec

#if defined(COREDB_ENABLE_JIT)
#include "coredb/jit/query_jit.h"

namespace coredb::exec {

// Same segment-pruning + per-segment loop as RunVectorized, but the inner
// filter+sum loop is the LLVM-compiled fused function instead of a SIMD
// kernel call.
AggregateResult RunJit(const std::vector<const storage::Segment*>& segments, const AggregateQuery& query,
                        const jit::QueryJit& compiled);

}  // namespace coredb::exec
#endif
