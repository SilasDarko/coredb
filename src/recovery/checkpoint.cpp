#include "coredb/recovery/checkpoint.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace coredb::recovery {

namespace fs = std::filesystem;

bool WriteCheckpoint(table::Table& table, const std::string& dir, const std::string& manifest_path,
                      uint64_t wal_lsn, std::string* error) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    *error = "failed to create checkpoint directory '" + dir + "': " + ec.message();
    return false;
  }

  table.Compact();  // best-effort: push everything decidable out of delta

  const auto segments = table.base_segments();
  std::vector<std::string> filenames;
  for (const auto& seg : segments) {
    const std::string filename = "segment_" + std::to_string(seg->segment_id()) + ".seg";
    const std::string full_path = (fs::path(dir) / filename).string();
    if (!seg->SaveToFile(full_path, error)) return false;
    filenames.push_back(filename);
  }

  std::ofstream out(manifest_path, std::ios::trunc);
  if (!out) {
    *error = "failed to open manifest '" + manifest_path + "' for writing";
    return false;
  }
  out << "LSN " << wal_lsn << "\n";
  for (const auto& f : filenames) out << f << "\n";
  return static_cast<bool>(out);
}

std::optional<CheckpointManifest> LoadManifest(const std::string& manifest_path, std::string* error) {
  std::ifstream in(manifest_path);
  if (!in) {
    *error = "no checkpoint manifest at '" + manifest_path + "'";
    return std::nullopt;
  }

  CheckpointManifest manifest;
  std::string first_line;
  if (!std::getline(in, first_line)) {
    *error = "empty manifest file";
    return std::nullopt;
  }
  std::istringstream iss(first_line);
  std::string tag;
  iss >> tag >> manifest.last_lsn;
  if (tag != "LSN") {
    *error = "manifest missing LSN header";
    return std::nullopt;
  }

  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty()) manifest.segment_files.push_back(line);
  }
  return manifest;
}

}  // namespace coredb::recovery
