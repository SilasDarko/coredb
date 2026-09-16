#include "coredb/table/table.h"

#include <unordered_map>
#include <unordered_set>

namespace coredb::table {

using mvcc::DeltaRecord;
using mvcc::IsVisible;
using mvcc::Transaction;
using mvcc::TxnState;
using mvcc::TxnStatus;
using mvcc::WriteResult;
using storage::Row;
using storage::Value;

namespace {

Row ExtractFullRow(const storage::Segment& seg, size_t row_idx) {
  Row row;
  row.reserve(seg.num_columns());
  for (size_t c = 0; c < seg.num_columns(); ++c) row.push_back(seg.column(c).ValueAt(row_idx));
  return row;
}

}  // namespace

Table::Table(std::string name, storage::Schema user_schema)
    : name_(std::move(name)), user_schema_(std::move(user_schema)) {
  full_schema_.push_back({kRowIdColumnName, storage::ColumnType::kInt64});
  for (const auto& col : user_schema_) full_schema_.push_back(col);
}

uint64_t Table::Insert(Transaction& txn, Row user_row) {
  const uint64_t row_id = next_row_id_.fetch_add(1);
  std::shared_lock lock(table_mu_);
  delta_.Insert(row_id, txn.txn_id, std::move(user_row));
  return row_id;
}

uint64_t Table::InsertWithRowId(Transaction& txn, uint64_t row_id, Row user_row) {
  std::shared_lock lock(table_mu_);
  delta_.Insert(row_id, txn.txn_id, std::move(user_row));
  uint64_t expected = next_row_id_.load();
  while (expected <= row_id && !next_row_id_.compare_exchange_weak(expected, row_id + 1)) {
  }
  return row_id;
}

bool Table::Update(Transaction& txn, uint64_t row_id, Row new_user_row) {
  std::shared_lock lock(table_mu_);
  const bool exists_in_base = RowIdExistsInBase(row_id);
  const WriteResult result = delta_.ClaimOrInsertBaseTombstone(row_id, txn, txn_mgr_, exists_in_base);
  if (result != WriteResult::kOk) return false;
  delta_.Insert(row_id, txn.txn_id, std::move(new_user_row));
  return true;
}

bool Table::Delete(Transaction& txn, uint64_t row_id) {
  std::shared_lock lock(table_mu_);
  const bool exists_in_base = RowIdExistsInBase(row_id);
  const WriteResult result = delta_.ClaimOrInsertBaseTombstone(row_id, txn, txn_mgr_, exists_in_base);
  return result == WriteResult::kOk;
}

bool Table::RowIdExistsInBase(uint64_t row_id) const {
  for (const auto& seg : base_segments_) {
    const int64_t* ids = seg->column(kRowIdColumnIndex).AsInt64();
    for (size_t r = 0; r < seg->num_rows(); ++r) {
      if (static_cast<uint64_t>(ids[r]) == row_id) return true;
    }
  }
  return false;
}

ScanSnapshot Table::SnapshotForScan(const Transaction& reader) const {
  std::shared_lock lock(table_mu_);
  ScanSnapshot snap;
  snap.segments = base_segments_;
  snap.delta_records = delta_.Snapshot();
  snap.reader = reader;
  return snap;
}

std::vector<Row> Table::MaterializeVisibleRows(const ScanSnapshot& snapshot) const {
  std::unordered_map<uint64_t, Row> from_delta;
  // A base row is only shadowed once delta has something *this reader can
  // actually see* for its row_id — a tombstone or update committed after
  // the reader's snapshot must not hide the still-valid base value (that
  // would break active-snapshot protection for exactly the readers
  // compaction is supposed to protect).
  std::unordered_set<uint64_t> suppress_base;

  for (const DeltaRecord& rec : snapshot.delta_records) {
    if (rec.is_base_tombstone) {
      bool delete_visible;
      if (rec.deleted_txn == snapshot.reader.txn_id) {
        delete_visible = true;
      } else {
        const TxnStatus st = txn_mgr_.GetStatus(rec.deleted_txn);
        delete_visible = st.state == TxnState::kCommitted && st.commit_ts <= snapshot.reader.snapshot_ts;
      }
      if (delete_visible) suppress_base.insert(rec.row_id);
      continue;
    }
    if (IsVisible(rec, snapshot.reader, txn_mgr_)) {
      from_delta[rec.row_id] = rec.row;
      suppress_base.insert(rec.row_id);
    }
  }

  std::vector<Row> out;
  out.reserve(from_delta.size());
  for (auto& [row_id, row] : from_delta) out.push_back(std::move(row));

  for (const auto& seg : snapshot.segments) {
    const int64_t* ids = seg->column(kRowIdColumnIndex).AsInt64();
    for (size_t r = 0; r < seg->num_rows(); ++r) {
      const uint64_t row_id = static_cast<uint64_t>(ids[r]);
      if (suppress_base.count(row_id)) continue;
      Row full = ExtractFullRow(*seg, r);
      out.emplace_back(full.begin() + 1, full.end());  // drop hidden row_id column
    }
  }
  return out;
}

void Table::Compact() {
  using clock = std::chrono::steady_clock;
  const auto start = clock::now();

  std::unique_lock lock(table_mu_);
  const uint64_t oldest_active = txn_mgr_.OldestActiveSnapshot();
  std::vector<DeltaRecord> snapshot = delta_.Snapshot();

  std::unordered_map<uint64_t, size_t> head_index;
  for (size_t i = 0; i < snapshot.size(); ++i) head_index[snapshot[i].row_id] = i;

  std::unordered_map<uint64_t, Row> live_stable_rows;
  std::unordered_set<uint64_t> stably_deleted_row_ids;
  std::vector<DeltaRecord> retained;
  retained.reserve(snapshot.size());

  for (size_t i = 0; i < snapshot.size(); ++i) {
    const DeltaRecord& rec = snapshot[i];
    const bool is_head = head_index[rec.row_id] == i;

    if (!rec.is_base_tombstone) {
      const TxnStatus creator = txn_mgr_.GetStatus(rec.created_txn);
      if (creator.state == TxnState::kAborted) {
        continue;  // never became visible to anyone; safe to drop unconditionally
      }
    }

    if (!is_head) {
      if (rec.deleted_txn != 0) {
        const TxnStatus deleter = txn_mgr_.GetStatus(rec.deleted_txn);
        if (deleter.state == TxnState::kCommitted && deleter.commit_ts <= oldest_active) {
          continue;  // stably superseded, no reader can ever need it again
        }
      }
      retained.push_back(rec);
      continue;
    }

    if (rec.is_base_tombstone) {
      const TxnStatus deleter = txn_mgr_.GetStatus(rec.deleted_txn);
      if (deleter.state == TxnState::kCommitted && deleter.commit_ts <= oldest_active) {
        stably_deleted_row_ids.insert(rec.row_id);
        continue;  // purge happens in the base rewrite below
      }
      if (deleter.state == TxnState::kAborted) {
        continue;  // the delete attempt never happened; base row stays as-is
      }
      retained.push_back(rec);
      continue;
    }

    const TxnStatus creator = txn_mgr_.GetStatus(rec.created_txn);
    const bool created_stable = creator.state == TxnState::kCommitted && creator.commit_ts <= oldest_active;
    if (!created_stable) {
      retained.push_back(rec);
      continue;
    }
    if (rec.deleted_txn == 0) {
      live_stable_rows[rec.row_id] = rec.row;  // promoted into the rebuilt base below
      continue;
    }
    const TxnStatus deleter = txn_mgr_.GetStatus(rec.deleted_txn);
    if (deleter.state == TxnState::kCommitted && deleter.commit_ts <= oldest_active) {
      continue;  // stably created AND stably superseded: fully retire
    }
    retained.push_back(rec);
  }

  const size_t rows_promoted = live_stable_rows.size();
  const size_t rows_retired = snapshot.size() - retained.size() - rows_promoted;

  if (!live_stable_rows.empty() || !stably_deleted_row_ids.empty()) {
    std::vector<Row> merged_rows;
    for (const auto& seg : base_segments_) {
      const int64_t* ids = seg->column(kRowIdColumnIndex).AsInt64();
      for (size_t r = 0; r < seg->num_rows(); ++r) {
        const uint64_t row_id = static_cast<uint64_t>(ids[r]);
        if (stably_deleted_row_ids.count(row_id)) continue;
        merged_rows.push_back(ExtractFullRow(*seg, r));
      }
    }
    for (auto& [row_id, user_row] : live_stable_rows) {
      Row full_row;
      full_row.reserve(user_row.size() + 1);
      full_row.emplace_back(static_cast<int64_t>(row_id));
      for (auto& v : user_row) full_row.push_back(std::move(v));
      merged_rows.push_back(std::move(full_row));
    }
    const uint64_t segment_id = next_segment_id_.fetch_add(1);
    auto new_segment = std::make_shared<storage::Segment>(
        storage::Segment::Build(segment_id, full_schema_, merged_rows));
    base_segments_ = {std::move(new_segment)};
  }

  delta_.ReplaceAll(std::move(retained));

  const auto duration = clock::now() - start;
  std::lock_guard<std::mutex> mlock(metrics_mu_);
  metrics_.runs += 1;
  metrics_.rows_promoted += rows_promoted;
  metrics_.rows_retired += rows_retired;
  metrics_.last_duration = duration;
  metrics_.total_duration += duration;
}

CompactionMetrics Table::compaction_metrics() const {
  std::lock_guard<std::mutex> lock(metrics_mu_);
  return metrics_;
}

size_t Table::num_base_segments() const {
  std::shared_lock lock(table_mu_);
  return base_segments_.size();
}

size_t Table::total_base_rows() const {
  std::shared_lock lock(table_mu_);
  size_t total = 0;
  for (const auto& seg : base_segments_) total += seg->num_rows();
  return total;
}

std::vector<std::shared_ptr<const storage::Segment>> Table::base_segments() const {
  std::shared_lock lock(table_mu_);
  return base_segments_;
}

void Table::LoadBaseSegments(std::vector<std::shared_ptr<const storage::Segment>> segments) {
  std::unique_lock lock(table_mu_);
  base_segments_ = std::move(segments);
  for (const auto& seg : base_segments_) {
    const int64_t* ids = seg->column(kRowIdColumnIndex).AsInt64();
    for (size_t r = 0; r < seg->num_rows(); ++r) {
      const uint64_t row_id = static_cast<uint64_t>(ids[r]);
      uint64_t expected = next_row_id_.load();
      while (expected <= row_id && !next_row_id_.compare_exchange_weak(expected, row_id + 1)) {
      }
    }
    const uint64_t segment_id = seg->segment_id();
    uint64_t expected_seg = next_segment_id_.load();
    while (expected_seg <= segment_id && !next_segment_id_.compare_exchange_weak(expected_seg, segment_id + 1)) {
    }
  }
}

}  // namespace coredb::table
