#include "coredb/mvcc/delta.h"

namespace coredb::mvcc {

bool IsVisible(const DeltaRecord& rec, const Transaction& reader, const TransactionManager& txm) {
  bool created_visible;
  if (rec.created_txn == 0) {
    created_visible = true;  // base-origin sentinel: always existed
  } else if (rec.created_txn == reader.txn_id) {
    created_visible = true;  // read-your-own-writes, even before commit
  } else {
    const TxnStatus st = txm.GetStatus(rec.created_txn);
    created_visible = st.state == TxnState::kCommitted && st.commit_ts <= reader.snapshot_ts;
  }
  if (!created_visible) return false;

  if (rec.deleted_txn == 0) return true;

  bool deleted_visible;
  if (rec.deleted_txn == reader.txn_id) {
    deleted_visible = true;  // I deleted it myself, so it's gone as far as I'm concerned
  } else {
    const TxnStatus st = txm.GetStatus(rec.deleted_txn);
    deleted_visible = st.state == TxnState::kCommitted && st.commit_ts <= reader.snapshot_ts;
  }
  return !deleted_visible;
}

void DeltaLayer::Insert(uint64_t row_id, uint64_t created_txn, storage::Row row) {
  std::unique_lock lock(mu_);
  records_.push_back(DeltaRecord{row_id, created_txn, 0, std::move(row), false});
}

WriteResult DeltaLayer::ClaimOrInsertBaseTombstone(uint64_t row_id, const Transaction& writer,
                                                    const TransactionManager& txm,
                                                    bool exists_in_base) {
  std::unique_lock lock(mu_);
  bool touched = false;
  for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
    if (it->row_id != row_id) continue;
    touched = true;

    if (it->deleted_txn != 0 && it->deleted_txn != writer.txn_id) {
      const TxnStatus st = txm.GetStatus(it->deleted_txn);
      if (st.state == TxnState::kActive) return WriteResult::kConflict;
      if (st.state == TxnState::kCommitted) continue;  // retired for good; older history can't help
      // kAborted: that claim never really happened — fall through and reclaim.
    } else if (it->deleted_txn == writer.txn_id) {
      continue;  // writer already deleted this exact version in this txn
    }

    if (!IsVisible(*it, writer, txm)) continue;
    it->deleted_txn = writer.txn_id;
    return WriteResult::kOk;
  }

  if (!touched && exists_in_base) {
    records_.push_back(DeltaRecord{row_id, /*created_txn=*/0, writer.txn_id, {}, /*is_base_tombstone=*/true});
    return WriteResult::kOk;
  }
  return WriteResult::kNotFound;
}

size_t DeltaLayer::size() const {
  std::shared_lock lock(mu_);
  return records_.size();
}

std::vector<DeltaRecord> DeltaLayer::Snapshot() const {
  std::shared_lock lock(mu_);
  return records_;
}

void DeltaLayer::ReplaceAll(std::vector<DeltaRecord> records) {
  std::unique_lock lock(mu_);
  records_ = std::move(records);
}

}  // namespace coredb::mvcc
