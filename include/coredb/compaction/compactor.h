#pragma once

#include <atomic>
#include <chrono>
#include <thread>

#include "coredb/table/table.h"

namespace coredb::compaction {

struct CompactorConfig {
  size_t delta_row_threshold = 1000;  // trigger a compaction once delta_size() reaches this
  std::chrono::milliseconds poll_interval{20};
};

// Runs Table::Compact() on a background thread whenever the delta layer
// crosses `delta_row_threshold`, so callers get LSM-style "compact while
// you keep writing" behavior without managing a thread themselves. Table's
// own synchronous Compact() remains the source of truth used by
// deterministic tests.
class BackgroundCompactor {
 public:
  BackgroundCompactor(table::Table& table, CompactorConfig config);
  ~BackgroundCompactor();

  BackgroundCompactor(const BackgroundCompactor&) = delete;
  BackgroundCompactor& operator=(const BackgroundCompactor&) = delete;

  void Stop();

 private:
  void Run();

  table::Table& table_;
  CompactorConfig config_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace coredb::compaction
