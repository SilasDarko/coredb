#include <algorithm>

#include "coredb/exec/query.h"

namespace coredb::exec {

AggregateResult RunVectorized(const std::vector<const storage::Segment*>& segments, const AggregateQuery& query,
                               size_t batch_size) {
  AggregateResult result;
  for (const storage::Segment* seg : segments) {
    if (seg->CanSkip(query.predicate_column, storage::PredicateOp::kGt, query.threshold)) {
      continue;  // segment pruning, same as the Volcano path
    }
    const int64_t* pred = seg->column(query.predicate_column).AsInt64();
    const int64_t* agg = seg->column(query.aggregate_column).AsInt64();
    const size_t num_rows = seg->num_rows();

    for (size_t offset = 0; offset < num_rows; offset += batch_size) {
      const size_t count = std::min(batch_size, num_rows - offset);
      const simd::FilterSumResult batch_result =
          simd::FilterGtSum(pred + offset, agg + offset, count, query.threshold);
      result.matched_count += batch_result.matched_count;
      result.sum += batch_result.sum;
    }
  }
  return result;
}

}  // namespace coredb::exec
