#pragma once

#include <string>
#include <vector>

#include "coredb/wal/record.h"

namespace coredb::wal {

struct WalReadResult {
  std::vector<WalRecord> records;
  bool truncated_tail = false;  // a record's declared length ran past EOF
  bool corrupt_record = false;  // a full-length record's checksum didn't match
  uint64_t bytes_read = 0;
  uint64_t last_valid_lsn = 0;
};

// Reads every well-formed record from `path`, starting from the beginning
// of the file. Stops at the first truncated or corrupt record (CoreDB's
// recovery policy is "trust nothing after the first bad record" — see
// DESIGN_DECISIONS.md) rather than trying to skip past damage.
WalReadResult ReadAll(const std::string& path);

}  // namespace coredb::wal
