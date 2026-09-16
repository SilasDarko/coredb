#pragma once

#include <memory>
#include <string>

#include "coredb/recovery/recovery_manager.h"
#include "coredb/table/table.h"
#include "coredb/wal/wal_writer.h"

namespace coredb::db {

// Ties Table + WAL + recovery into the API tests and benchmarks actually
// drive: opening a Database runs recovery automatically (replaying any WAL
// left from a previous, uncleanly-ended process), and every write goes
// through the WAL before it's committed.
class Database {
 public:
  Database(std::string data_dir, storage::Schema user_schema,
            wal::WalWriterConfig wal_config = wal::WalWriterConfig{});

  table::Table& table() { return table_; }
  const recovery::RecoveryStats& recovery_stats() const { return recovery_stats_; }

  mvcc::Transaction Begin();
  uint64_t Insert(mvcc::Transaction& txn, storage::Row row);
  bool Update(mvcc::Transaction& txn, uint64_t row_id, storage::Row new_row);
  bool Delete(mvcc::Transaction& txn, uint64_t row_id);
  void Commit(mvcc::Transaction& txn);
  void Abort(mvcc::Transaction& txn);

  // Compacts, writes base segments + manifest under data_dir/checkpoint,
  // recorded against the WAL's current LSN.
  bool Checkpoint(std::string* error);

  std::string wal_path() const;
  std::string checkpoint_dir() const;
  std::string manifest_path() const;

 private:
  std::string data_dir_;
  table::Table table_;
  recovery::RecoveryStats recovery_stats_;
  std::unique_ptr<wal::WalWriter> wal_writer_;
};

}  // namespace coredb::db
