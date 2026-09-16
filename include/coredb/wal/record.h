#pragma once

#include <cstdint>
#include <vector>

#include "coredb/storage/types.h"

namespace coredb::wal {

enum class WalRecordType : uint8_t {
  kBegin = 1,
  kInsert = 2,
  kUpdate = 3,
  kDelete = 4,
  kCommit = 5,
  kAbort = 6,
};

const char* ToString(WalRecordType type);

// A parsed WAL record. `row` is only meaningful for kInsert/kUpdate;
// `row_id` for kInsert/kUpdate/kDelete.
struct WalRecord {
  uint64_t lsn = 0;
  uint64_t txn_id = 0;
  WalRecordType type = WalRecordType::kBegin;
  uint64_t row_id = 0;
  storage::Row row;
};

// Encodes a record's body (everything the on-disk checksum covers: lsn,
// txn_id, type, and payload) — used by both WalWriter (to checksum before
// writing) and tests that want to hand-corrupt bytes.
std::vector<uint8_t> EncodeBody(const WalRecord& record);
bool DecodeBody(const std::vector<uint8_t>& body, WalRecord* out);

}  // namespace coredb::wal
