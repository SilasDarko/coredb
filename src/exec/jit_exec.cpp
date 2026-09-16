#include "coredb/exec/query.h"

namespace coredb::exec {

AggregateResult RunJit(const std::vector<const storage::Segment*>& segments, const AggregateQuery& query,
                        const jit::QueryJit& compiled) {
  AggregateResult result;
  for (const storage::Segment* seg : segments) {
    if (seg->CanSkip(query.predicate_column, storage::PredicateOp::kGt, query.threshold)) {
      continue;
    }
    const int64_t* pred = seg->column(query.predicate_column).AsInt64();
    const int64_t* agg = seg->column(query.aggregate_column).AsInt64();
    const simd::FilterSumResult seg_result = compiled.Run(pred, agg, seg->num_rows(), query.threshold);
    result.matched_count += seg_result.matched_count;
    result.sum += seg_result.sum;
  }
  return result;
}

}  // namespace coredb::exec
