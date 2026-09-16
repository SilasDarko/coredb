#pragma once

#include <cstdint>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace coredb::mvcc {

enum class TxnState : uint8_t { kActive, kCommitted, kAborted };

// A transaction handle. `txn_id` identifies the transaction and doubles as
// its "creation stamp" for rows it writes; `snapshot_ts` is the commit-order
// watermark this transaction reads as of (snapshot isolation: it sees every
// write committed at or before snapshot_ts, and nothing committed after).
struct Transaction {
  uint64_t txn_id = 0;
  uint64_t snapshot_ts = 0;
  uint64_t commit_ts = 0;
  TxnState state = TxnState::kActive;
};

struct TxnStatus {
  TxnState state = TxnState::kActive;
  uint64_t commit_ts = 0;
};

// Hands out transaction ids and commit timestamps from a single monotonic
// counter shared by both (a deliberate simplification: begin-order and
// commit-order live in the same namespace, so "is this commit visible to
// that snapshot" is a plain integer comparison — see DESIGN_DECISIONS.md).
// Tracks which snapshots are still in use so the compactor never reclaims a
// row version an active reader might still need.
class TransactionManager {
 public:
  Transaction Begin();
  void Commit(Transaction& txn);
  void Abort(Transaction& txn);

  // Status of any txn_id this manager has ever seen. A txn_id it has never
  // seen (or one still active) reports kActive/commit_ts=0, which visibility
  // checks treat as "not yet committed" — correct either way.
  TxnStatus GetStatus(uint64_t txn_id) const;

  // The oldest snapshot_ts among currently-active transactions, or the
  // latest commit_ts if none are active. Compaction must not discard any
  // row version still needed by a reader at or below this watermark.
  uint64_t OldestActiveSnapshot() const;

  size_t ActiveTransactionCount() const;

 private:
  mutable std::mutex mu_;
  uint64_t next_id_ = 1;
  uint64_t last_commit_ts_ = 0;
  std::multiset<uint64_t> active_snapshots_;
  std::unordered_set<uint64_t> active_txn_ids_;
  std::unordered_map<uint64_t, TxnStatus> status_;
};

}  // namespace coredb::mvcc
