#pragma once

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "coredb/util/cpu_features.h"

namespace coredb::bench {

inline void PrintBanner(const char* name) {
  const auto& caps = util::DetectCapabilities();
  std::fprintf(stderr, "=== CoreDB %s ===\n%s\n%s\n", name, util::DescribeCapabilities(caps).c_str(),
               std::string(20 + std::strlen(name), '=').c_str());
}

// CLI benchmarks (as opposed to the Google Benchmark ones) write their own
// structured results here so a full run leaves a record on disk instead of
// only stdout. Kept out of git (see .gitignore) since it's machine- and
// run-specific, not a build artifact.
inline std::ofstream OpenResultsFile(const std::string& name) {
  std::filesystem::create_directories("results");
  std::ofstream out("results/" + name + ".jsonl", std::ios::app);
  return out;
}

}  // namespace coredb::bench
