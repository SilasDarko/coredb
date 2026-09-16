#include "coredb/mvcc/transaction.h"

namespace coredb::mvcc {

Transaction TransactionManager::Begin() {
  std::lock_guard<std::mutex> lock(mu_);
  Transaction txn;
  txn.txn_id = next_id_++;
  txn.snapshot_ts = last_commit_ts_;
  txn.state = TxnState::kActive;
  active_snapshots_.insert(txn.snapshot_ts);
  active_txn_ids_.insert(txn.txn_id);
  return txn;
}

void TransactionManager::Commit(Transaction& txn) {
  std::lock_guard<std::mutex> lock(mu_);
  txn.commit_ts = next_id_++;
  txn.state = TxnState::kCommitted;
  status_[txn.txn_id] = TxnStatus{TxnState::kCommitted, txn.commit_ts};
  last_commit_ts_ = txn.commit_ts;
  active_txn_ids_.erase(txn.txn_id);
  active_snapshots_.erase(active_snapshots_.find(txn.snapshot_ts));
}

void TransactionManager::Abort(Transaction& txn) {
  std::lock_guard<std::mutex> lock(mu_);
  txn.state = TxnState::kAborted;
  status_[txn.txn_id] = TxnStatus{TxnState::kAborted, 0};
  active_txn_ids_.erase(txn.txn_id);
  active_snapshots_.erase(active_snapshots_.find(txn.snapshot_ts));
}

TxnStatus TransactionManager::GetStatus(uint64_t txn_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = status_.find(txn_id);
  if (it == status_.end()) return TxnStatus{TxnState::kActive, 0};
  return it->second;
}

uint64_t TransactionManager::OldestActiveSnapshot() const {
  std::lock_guard<std::mutex> lock(mu_);
  if (active_snapshots_.empty()) return last_commit_ts_;
  return *active_snapshots_.begin();
}

size_t TransactionManager::ActiveTransactionCount() const {
  std::lock_guard<std::mutex> lock(mu_);
  return active_txn_ids_.size();
}

}  // namespace coredb::mvcc
