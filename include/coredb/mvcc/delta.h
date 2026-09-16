#pragma once

#include <cstdint>
#include <shared_mutex>
#include <vector>

#include "coredb/mvcc/transaction.h"
#include "coredb/storage/types.h"

namespace coredb::mvcc {

// One row version living in the delta layer, keyed by the table's internal
// row_id (see table/table.h — every row, base or delta, carries a hidden
// leading row_id column so updates/deletes can address a specific logical
// row regardless of which layer currently holds its latest version).
//
// created_txn == 0 is a sentinel meaning "not created by a tracked
// transaction" — used only by tombstone records that mark a *base-segment*
// row deleted, since base rows predate MVCC tracking and are otherwise
// unconditionally visible.
struct DeltaRecord {
  uint64_t row_id = 0;
  uint64_t created_txn = 0;
  uint64_t deleted_txn = 0;  // 0 == not (yet) superseded
  storage::Row row;          // empty for tombstones
  bool is_base_tombstone = false;
};

enum class WriteResult { kOk, kNotFound, kConflict };

// True if `rec` is visible to a transaction reading at `reader`, per
// snapshot-isolation rules: see its own uncommitted writes, and otherwise
// only writes committed at or before its snapshot_ts. created_txn == 0
// (base-origin rows) is always considered created/visible.
bool IsVisible(const DeltaRecord& rec, const Transaction& reader, const TransactionManager& txm);

// Append-only, thread-safe staging area for row versions that haven't (yet)
// been folded into an immutable base Segment. This is the "mutable" half of
// CoreDB's LSM-style storage; see compaction/compactor.h for how records
// migrate out of it.
class DeltaLayer {
 public:
  // Plain append for a brand-new row_id (Table::Insert). No conflict is
  // possible since the row_id is freshly allocated.
  void Insert(uint64_t row_id, uint64_t created_txn, storage::Row row);

  // Used by Table::Update/Delete. Finds the newest live version of row_id
  // already in the delta layer and marks it superseded by `writer`,
  // OR — if row_id has never been touched by the delta layer before and
  // `exists_in_base` is true — appends a tombstone claiming the base row.
  //
  // Returns kConflict if a *different*, still-active transaction currently
  // holds an unresolved claim on the same row (CoreDB aborts on write-write
  // conflict rather than blocking or queuing). Returns kNotFound if row_id
  // has no live version anywhere. The whole decision runs under one lock so
  // two concurrent first-touches of the same base row can never both
  // succeed.
  WriteResult ClaimOrInsertBaseTombstone(uint64_t row_id, const Transaction& writer,
                                          const TransactionManager& txm, bool exists_in_base);

  size_t size() const;

  // Copies the current records under a shared lock so callers (scans,
  // compaction) can iterate without holding the layer locked — a
  // deliberate throughput-over-memory trade-off documented in
  // DESIGN_DECISIONS.md; see that file before assuming this scales to a
  // huge resident delta.
  std::vector<DeltaRecord> Snapshot() const;

  // Replaces the contents wholesale; used by the compactor after it has
  // decided which records survive.
  void ReplaceAll(std::vector<DeltaRecord> records);

 private:
  mutable std::shared_mutex mu_;
  std::vector<DeltaRecord> records_;
};

}  // namespace coredb::mvcc
