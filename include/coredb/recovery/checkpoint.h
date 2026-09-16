#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "coredb/table/table.h"

namespace coredb::recovery {

// A checkpoint records which WAL LSN its saved segment files already
// reflect: recovery only needs to replay records after `last_lsn`.
//
// Simplifying assumption (documented in DESIGN_DECISIONS.md): CoreDB only
// takes checkpoints at a quiescent point (no in-flight transactions), so
// "everything up to last_lsn is captured in these segment files" is exactly
// true. A production engine would instead support fuzzy checkpoints that
// can be taken concurrently with writers.
struct CheckpointManifest {
  uint64_t last_lsn = 0;
  std::vector<std::string> segment_files;  // paths, relative to the manifest's directory
};

// Compacts `table` (best effort) and writes its current base segments to
// `dir`, then writes the manifest itself to `manifest_path`. `wal_lsn` is
// the WAL's last_lsn() at the moment of the checkpoint.
bool WriteCheckpoint(table::Table& table, const std::string& dir, const std::string& manifest_path,
                      uint64_t wal_lsn, std::string* error);

std::optional<CheckpointManifest> LoadManifest(const std::string& manifest_path, std::string* error);

}  // namespace coredb::recovery
