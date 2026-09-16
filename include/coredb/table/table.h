#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

#include "coredb/mvcc/delta.h"
#include "coredb/mvcc/transaction.h"
#include "coredb/storage/segment.h"

namespace coredb::table {

// Every table gets an implicit leading column so an update/delete can
// address a specific logical row no matter which storage layer currently
// holds its latest version. It is never exposed through the user-facing
// schema/Row API.
constexpr char kRowIdColumnName[] = "__row_id__";
constexpr size_t kRowIdColumnIndex = 0;

struct CompactionMetrics {
  uint64_t runs = 0;
  uint64_t rows_promoted = 0;
  uint64_t rows_retired = 0;
  std::chrono::nanoseconds last_duration{0};
  std::chrono::nanoseconds total_duration{0};
};

// A consistent point-in-time view used by the execution engine: an
// immutable list of base segments plus a copy of the delta records, both
// taken under the same lock so a scan never observes a compaction
// half-applied.
struct ScanSnapshot {
  std::vector<std::shared_ptr<const storage::Segment>> segments;
  std::vector<mvcc::DeltaRecord> delta_records;
  mvcc::Transaction reader;
};

// Ties storage (immutable Segments), the MVCC delta layer, and the
// transaction manager together into the row-level read/write API the
// execution engine and tests build on. See DESIGN_DECISIONS.md for the
// concurrency model (a shared_mutex where compaction takes the exclusive
// side) and its trade-offs.
class Table {
 public:
  Table(std::string name, storage::Schema user_schema);

  const std::string& name() const { return name_; }
  const storage::Schema& user_schema() const { return user_schema_; }
  const storage::Schema& full_schema() const { return full_schema_; }

  mvcc::Transaction Begin() { return txn_mgr_.Begin(); }
  void Commit(mvcc::Transaction& txn) { txn_mgr_.Commit(txn); }
  void Abort(mvcc::Transaction& txn) { txn_mgr_.Abort(txn); }
  mvcc::TransactionManager& transaction_manager() { return txn_mgr_; }

  // Returns the new row's internal row_id.
  uint64_t Insert(mvcc::Transaction& txn, storage::Row user_row);
  // Like Insert, but for a caller-chosen row_id (WAL redo replay, where the
  // row_id must match what was originally logged). Bumps the row_id
  // allocator past `row_id` so subsequent normal Inserts never collide.
  uint64_t InsertWithRowId(mvcc::Transaction& txn, uint64_t row_id, storage::Row user_row);
  // False on conflict (a concurrent, still-active txn holds this row) or if
  // row_id does not currently exist.
  bool Update(mvcc::Transaction& txn, uint64_t row_id, storage::Row new_user_row);
  bool Delete(mvcc::Transaction& txn, uint64_t row_id);

  ScanSnapshot SnapshotForScan(const mvcc::Transaction& reader) const;

  // Merges a ScanSnapshot's segments + delta into user-visible rows
  // (row_id column stripped). Exposed as a free function on Table because
  // both the reference/test path and the execution engine's ScanOp need
  // the exact same merge semantics.
  std::vector<storage::Row> MaterializeVisibleRows(const ScanSnapshot& snapshot) const;

  // Synchronous; safe to call directly from tests/benchmarks for
  // deterministic compaction, or drive via BackgroundCompactor (see
  // compaction/compactor.h) for a realistic always-on background thread.
  void Compact();
  CompactionMetrics compaction_metrics() const;

  size_t delta_size() const { return delta_.size(); }
  size_t num_base_segments() const;
  size_t total_base_rows() const;

  // Checkpoint/recovery support: a copy of the current base segment list,
  // and a way to install a set of segments loaded from a checkpoint before
  // any WAL replay happens (bypasses the normal Insert path entirely).
  std::vector<std::shared_ptr<const storage::Segment>> base_segments() const;
  void LoadBaseSegments(std::vector<std::shared_ptr<const storage::Segment>> segments);

 private:
  bool RowIdExistsInBase(uint64_t row_id) const;  // caller must hold table_mu_

  std::string name_;
  storage::Schema user_schema_;
  storage::Schema full_schema_;

  mvcc::TransactionManager txn_mgr_;
  mvcc::DeltaLayer delta_;

  mutable std::shared_mutex table_mu_;
  std::vector<std::shared_ptr<const storage::Segment>> base_segments_;
  std::atomic<uint64_t> next_row_id_{1};
  std::atomic<uint64_t> next_segment_id_{1};

  mutable std::mutex metrics_mu_;
  CompactionMetrics metrics_;
};

}  // namespace coredb::table
