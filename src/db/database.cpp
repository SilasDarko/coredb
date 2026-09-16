#include "coredb/db/database.h"

#include <filesystem>

#include "coredb/recovery/checkpoint.h"

namespace coredb::db {

namespace fs = std::filesystem;

namespace {
std::string JoinPath(const std::string& dir, const char* leaf) { return (fs::path(dir) / leaf).string(); }
}  // namespace

Database::Database(std::string data_dir, storage::Schema user_schema, wal::WalWriterConfig wal_config)
    : data_dir_(std::move(data_dir)), table_("db", std::move(user_schema)) {
  fs::create_directories(data_dir_);

  recovery::RecoveryConfig recovery_config;
  recovery_config.wal_path = wal_path();
  recovery_config.checkpoint_manifest_path = manifest_path();
  recovery_config.segment_dir = checkpoint_dir();
  recovery_config.num_workers = 1;
  recovery_stats_ = recovery::RecoveryManager::Recover(table_, recovery_config);

  wal_config.starting_lsn = recovery_stats_.last_wal_lsn + 1;
  wal_writer_ = std::make_unique<wal::WalWriter>(wal_path(), wal_config);
}

std::string Database::wal_path() const { return JoinPath(data_dir_, "wal.log"); }
std::string Database::checkpoint_dir() const { return JoinPath(data_dir_, "checkpoint"); }
std::string Database::manifest_path() const { return JoinPath(checkpoint_dir(), "manifest.txt"); }

mvcc::Transaction Database::Begin() {
  mvcc::Transaction txn = table_.Begin();
  wal_writer_->Append(txn.txn_id, wal::WalRecordType::kBegin);
  return txn;
}

uint64_t Database::Insert(mvcc::Transaction& txn, storage::Row row) {
  const uint64_t row_id = table_.Insert(txn, row);
  wal_writer_->Append(txn.txn_id, wal::WalRecordType::kInsert, row_id, std::move(row));
  return row_id;
}

bool Database::Update(mvcc::Transaction& txn, uint64_t row_id, storage::Row new_row) {
  if (!table_.Update(txn, row_id, new_row)) return false;
  wal_writer_->Append(txn.txn_id, wal::WalRecordType::kUpdate, row_id, std::move(new_row));
  return true;
}

bool Database::Delete(mvcc::Transaction& txn, uint64_t row_id) {
  if (!table_.Delete(txn, row_id)) return false;
  wal_writer_->Append(txn.txn_id, wal::WalRecordType::kDelete, row_id, {});
  return true;
}

void Database::Commit(mvcc::Transaction& txn) {
  wal_writer_->Append(txn.txn_id, wal::WalRecordType::kCommit);
  table_.Commit(txn);
}

void Database::Abort(mvcc::Transaction& txn) {
  wal_writer_->Append(txn.txn_id, wal::WalRecordType::kAbort);
  table_.Abort(txn);
}

bool Database::Checkpoint(std::string* error) {
  wal_writer_->Flush();
  return recovery::WriteCheckpoint(table_, checkpoint_dir(), manifest_path(), wal_writer_->last_lsn(), error);
}

}  // namespace coredb::db
