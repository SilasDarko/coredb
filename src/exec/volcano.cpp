#include "coredb/exec/query.h"

namespace coredb::exec {

namespace {

// Classic Volcano/iterator-model operator: each Next() call does one
// virtual dispatch and produces (or fails to produce) a single row. This
// is intentionally the "slow" baseline — no batching, no SIMD, a virtual
// call per row per operator in the chain — see BENCHMARKS.md for the
// measured cost of that per-row overhead against RunVectorized/JIT.
class RowIterator {
 public:
  virtual ~RowIterator() = default;
  virtual bool Next() = 0;
  virtual int64_t Predicate() const = 0;
  virtual int64_t Aggregate() const = 0;
};

// Leaf operator: walks every row of every (non-pruned) segment, exposing
// the predicate and aggregate column values one row at a time.
class SegmentScanOp : public RowIterator {
 public:
  SegmentScanOp(const std::vector<const storage::Segment*>& segments, size_t pred_col, size_t agg_col,
                int64_t prune_threshold)
      : segments_(segments), pred_col_(pred_col), agg_col_(agg_col), prune_threshold_(prune_threshold) {}

  bool Next() override {
    while (true) {
      if (current_ == nullptr && !AdvanceSegment()) return false;
      if (row_ < current_->num_rows()) {
        pred_ = current_->column(pred_col_).AsInt64()[row_];
        agg_ = current_->column(agg_col_).AsInt64()[row_];
        ++row_;
        return true;
      }
      current_ = nullptr;
    }
  }
  int64_t Predicate() const override { return pred_; }
  int64_t Aggregate() const override { return agg_; }

 private:
  bool AdvanceSegment() {
    while (seg_idx_ < segments_.size()) {
      const storage::Segment* seg = segments_[seg_idx_++];
      if (seg->CanSkip(pred_col_, storage::PredicateOp::kGt, static_cast<int64_t>(prune_threshold_))) {
        continue;  // segment pruning: every row here is guaranteed to fail the predicate
      }
      current_ = seg;
      row_ = 0;
      return true;
    }
    return false;
  }

  const std::vector<const storage::Segment*>& segments_;
  size_t pred_col_;
  size_t agg_col_;
  int64_t prune_threshold_;
  size_t seg_idx_ = 0;
  const storage::Segment* current_ = nullptr;
  size_t row_ = 0;
  int64_t pred_ = 0;
  int64_t agg_ = 0;
};

class FilterOp : public RowIterator {
 public:
  FilterOp(RowIterator& child, int64_t threshold) : child_(child), threshold_(threshold) {}
  bool Next() override {
    while (child_.Next()) {
      if (child_.Predicate() > threshold_) return true;
    }
    return false;
  }
  int64_t Predicate() const override { return child_.Predicate(); }
  int64_t Aggregate() const override { return child_.Aggregate(); }

 private:
  RowIterator& child_;
  int64_t threshold_;
};

}  // namespace

AggregateResult RunVolcano(const std::vector<const storage::Segment*>& segments, const AggregateQuery& query) {
  SegmentScanOp scan(segments, query.predicate_column, query.aggregate_column, query.threshold);
  FilterOp filter(scan, query.threshold);

  AggregateResult result;
  while (filter.Next()) {
    result.sum += filter.Aggregate();
    result.matched_count += 1;
  }
  return result;
}

}  // namespace coredb::exec
