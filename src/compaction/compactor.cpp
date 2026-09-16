#include "coredb/compaction/compactor.h"

namespace coredb::compaction {

BackgroundCompactor::BackgroundCompactor(table::Table& table, CompactorConfig config)
    : table_(table), config_(config), thread_([this] { Run(); }) {}

BackgroundCompactor::~BackgroundCompactor() { Stop(); }

void BackgroundCompactor::Stop() {
  if (stop_.exchange(true)) return;  // already stopped
  if (thread_.joinable()) thread_.join();
}

void BackgroundCompactor::Run() {
  while (!stop_.load(std::memory_order_relaxed)) {
    if (table_.delta_size() >= config_.delta_row_threshold) {
      table_.Compact();
    }
    std::this_thread::sleep_for(config_.poll_interval);
  }
}

}  // namespace coredb::compaction
