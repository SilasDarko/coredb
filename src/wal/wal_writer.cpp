#include "coredb/wal/wal_writer.h"

#include <stdexcept>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "coredb/util/crc32c.h"

namespace coredb::wal {

const char* ToString(DurabilityMode mode) {
  switch (mode) {
    case DurabilityMode::kNoSync: return "no_sync";
    case DurabilityMode::kSyncEveryCommit: return "sync_every_commit";
    case DurabilityMode::kSyncEveryRecord: return "sync_every_record";
  }
  return "unknown";
}

WalWriter::WalWriter(std::string path, WalWriterConfig config)
    : path_(std::move(path)), config_(config), next_lsn_(config.starting_lsn) {
  file_ = std::fopen(path_.c_str(), "ab");
  if (!file_) {
    throw std::runtime_error("WalWriter: failed to open '" + path_ + "' for append");
  }
}

WalWriter::~WalWriter() {
  if (file_) {
    std::fflush(file_);
    std::fclose(file_);
  }
}

uint64_t WalWriter::AppendRecord(WalRecord record) {
  std::lock_guard<std::mutex> lock(mu_);
  record.lsn = next_lsn_.fetch_add(1);
  const std::vector<uint8_t> body = EncodeBody(record);
  const uint32_t checksum = util::Crc32c(body.data(), body.size());
  const uint32_t total_len = static_cast<uint32_t>(sizeof(checksum) + body.size());

  std::fwrite(&total_len, sizeof(total_len), 1, file_);
  std::fwrite(&checksum, sizeof(checksum), 1, file_);
  std::fwrite(body.data(), 1, body.size(), file_);

  const bool should_sync = config_.durability == DurabilityMode::kSyncEveryRecord ||
                            (config_.durability == DurabilityMode::kSyncEveryCommit &&
                             record.type == WalRecordType::kCommit);
  std::fflush(file_);
  if (should_sync) {
#if defined(_WIN32)
    // Not targeted by this build; fflush above is the best available.
#else
    fsync(fileno(file_));
#endif
  }
  return record.lsn;
}

uint64_t WalWriter::Append(uint64_t txn_id, WalRecordType type, uint64_t row_id, storage::Row row) {
  WalRecord record;
  record.txn_id = txn_id;
  record.type = type;
  record.row_id = row_id;
  record.row = std::move(row);
  return AppendRecord(std::move(record));
}

uint64_t WalWriter::Append(uint64_t txn_id, WalRecordType type) {
  WalRecord record;
  record.txn_id = txn_id;
  record.type = type;
  return AppendRecord(std::move(record));
}

void WalWriter::Flush() {
  std::lock_guard<std::mutex> lock(mu_);
  std::fflush(file_);
#if !defined(_WIN32)
  fsync(fileno(file_));
#endif
}

}  // namespace coredb::wal
