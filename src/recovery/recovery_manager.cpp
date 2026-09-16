#include "coredb/recovery/recovery_manager.h"

#include <algorithm>
#include <filesystem>
#include <thread>
#include <unordered_set>
#include <vector>

#include "coredb/recovery/checkpoint.h"
#include "coredb/wal/wal_reader.h"

namespace coredb::recovery {

namespace fs = std::filesystem;
using wal::WalRecord;
using wal::WalRecordType;

namespace {

void ApplyOp(table::Table& table, const WalRecord& op) {
  mvcc::Transaction txn = table.Begin();
  switch (op.type) {
    case WalRecordType::kInsert:
      table.InsertWithRowId(txn, op.row_id, op.row);
      break;
    case WalRecordType::kUpdate:
      table.Update(txn, op.row_id, op.row);
      break;
    case WalRecordType::kDelete:
      table.Delete(txn, op.row_id);
      break;
    default:
      break;
  }
  table.Commit(txn);
}

}  // namespace

RecoveryStats RecoveryManager::Recover(table::Table& table, const RecoveryConfig& config) {
  const auto start = std::chrono::steady_clock::now();
  RecoveryStats stats;
  stats.num_workers = std::max(1u, config.num_workers);

  uint64_t checkpoint_lsn = 0;
  std::string manifest_error;
  if (auto manifest = LoadManifest(config.checkpoint_manifest_path, &manifest_error)) {
    stats.checkpoint_found = true;
    stats.checkpoint_lsn = manifest->last_lsn;
    checkpoint_lsn = manifest->last_lsn;

    std::vector<std::shared_ptr<const storage::Segment>> segments;
    for (const auto& filename : manifest->segment_files) {
      const std::string full_path = (fs::path(config.segment_dir) / filename).string();
      std::string load_error;
      auto seg = storage::Segment::LoadFromFile(full_path, table.full_schema(), &load_error);
      if (!seg.has_value()) {
        stats.error = "checkpoint segment load failed: " + load_error;
        return stats;
      }
      segments.push_back(std::make_shared<storage::Segment>(std::move(*seg)));
    }
    table.LoadBaseSegments(std::move(segments));
  }
  // A missing checkpoint just means "replay everything from LSN 1", which
  // is the correct behavior for a table that has never been checkpointed.

  const wal::WalReadResult wal_result = wal::ReadAll(config.wal_path);
  stats.records_read = wal_result.records.size();
  stats.truncated_tail = wal_result.truncated_tail;
  stats.corrupt_tail = wal_result.corrupt_record;
  stats.last_wal_lsn = std::max(wal_result.last_valid_lsn, checkpoint_lsn);

  std::unordered_set<uint64_t> committed_txn_ids;
  for (const auto& rec : wal_result.records) {
    if (rec.type == WalRecordType::kCommit) committed_txn_ids.insert(rec.txn_id);
  }

  std::vector<WalRecord> ops;
  ops.reserve(wal_result.records.size());
  for (const auto& rec : wal_result.records) {
    if (rec.lsn <= checkpoint_lsn) continue;
    if (rec.type != WalRecordType::kInsert && rec.type != WalRecordType::kUpdate &&
        rec.type != WalRecordType::kDelete) {
      continue;
    }
    if (!committed_txn_ids.count(rec.txn_id)) continue;  // never committed: drop (implicit UNDO)
    ops.push_back(rec);
  }
  stats.committed_transactions = committed_txn_ids.size();
  stats.operations_replayed = ops.size();

  // Shard by row_id so each worker's sub-sequence for any given row stays
  // in original LSN order (the only ordering correctness actually depends
  // on), while unrelated rows replay fully in parallel.
  std::vector<std::vector<WalRecord>> shards(stats.num_workers);
  for (auto& op : ops) {
    shards[op.row_id % stats.num_workers].push_back(op);
  }

  std::vector<std::thread> workers;
  workers.reserve(stats.num_workers);
  for (unsigned w = 0; w < stats.num_workers; ++w) {
    workers.emplace_back([&table, &shards, w] {
      for (const auto& op : shards[w]) ApplyOp(table, op);
    });
  }
  for (auto& t : workers) t.join();

  for (const auto& seg : table.base_segments()) {
    std::string verify_error;
    if (!seg->VerifyIntegrity(&verify_error)) {
      stats.checksum_verification_passed = false;
      stats.error = verify_error;
    }
  }

  stats.duration = std::chrono::steady_clock::now() - start;
  return stats;
}

}  // namespace coredb::recovery
