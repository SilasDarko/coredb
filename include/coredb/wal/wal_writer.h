#pragma once

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

#include "coredb/wal/record.h"

namespace coredb::wal {

enum class DurabilityMode {
  kNoSync,           // rely on OS page cache only; fastest, least durable
  kSyncEveryCommit,  // fsync after every COMMIT record (typical DB default)
  kSyncEveryRecord,  // fsync after every single record; slowest, most durable
};

const char* ToString(DurabilityMode mode);

struct WalWriterConfig {
  DurabilityMode durability = DurabilityMode::kSyncEveryCommit;
  uint64_t starting_lsn = 1;  // set to checkpoint.last_lsn + 1 when resuming
};

// Append-only, single-file WAL writer. Every record is length-prefixed and
// checksummed independently so a reader can detect a truncated tail (a
// record whose declared length runs past EOF — the classic "crashed
// mid-write" shape) versus a corrupt record (full-length bytes present but
// the checksum doesn't match) versus a clean end of log.
class WalWriter {
 public:
  WalWriter(std::string path, WalWriterConfig config);
  ~WalWriter();

  WalWriter(const WalWriter&) = delete;
  WalWriter& operator=(const WalWriter&) = delete;

  // Appends a record, assigning it the next LSN. Returns the assigned LSN.
  uint64_t Append(uint64_t txn_id, WalRecordType type, uint64_t row_id, storage::Row row);
  uint64_t Append(uint64_t txn_id, WalRecordType type);  // BEGIN/COMMIT/ABORT convenience

  void Flush();  // force an fsync regardless of durability mode
  uint64_t last_lsn() const { return next_lsn_.load() - 1; }

 private:
  uint64_t AppendRecord(WalRecord record);

  std::string path_;
  WalWriterConfig config_;
  std::FILE* file_ = nullptr;
  std::mutex mu_;
  std::atomic<uint64_t> next_lsn_;
};

}  // namespace coredb::wal
