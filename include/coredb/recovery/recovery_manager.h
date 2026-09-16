#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include "coredb/table/table.h"

namespace coredb::recovery {

struct RecoveryConfig {
  std::string wal_path;
  std::string checkpoint_manifest_path;  // may not exist: full replay from LSN 1
  std::string segment_dir;               // directory holding checkpoint segment files
  unsigned num_workers = 1;              // parallel REDO apply workers, sharded by row_id
};

struct RecoveryStats {
  bool checkpoint_found = false;
  uint64_t checkpoint_lsn = 0;
  uint64_t records_read = 0;
  uint64_t committed_transactions = 0;
  uint64_t operations_replayed = 0;
  bool truncated_tail = false;
  bool corrupt_tail = false;
  uint64_t last_wal_lsn = 0;  // highest LSN seen in the WAL, valid or not; resume writing after this
  unsigned num_workers = 1;
  std::chrono::nanoseconds duration{0};
  bool checksum_verification_passed = true;
  std::string error;  // non-empty on a fatal failure (checkpoint load, etc.)
};

// Loads a checkpoint (if one exists) into `table`, then REDO-replays every
// WAL record after the checkpoint's LSN whose transaction has a COMMIT
// record — aborted and never-resolved (crash-in-progress) transactions are
// dropped, exactly as ARIES-style REDO+(implicit UNDO by omission) recovery
// intends. See DESIGN_DECISIONS.md for why replay is safe to parallelize
// by sharding on row_id, and why it doesn't preserve original transaction
// boundaries.
class RecoveryManager {
 public:
  static RecoveryStats Recover(table::Table& table, const RecoveryConfig& config);
};

}  // namespace coredb::recovery
