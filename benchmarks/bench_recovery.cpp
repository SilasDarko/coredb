// REDO recovery scalability: generate a WAL of a configurable size, then
// replay it from scratch with 1/2/4/8/16/32 REDO worker threads, reporting
// wall-clock time, MB/s, rows/sec, and speedup relative to the
// single-threaded run.
//
// Usage: bench_recovery [--size-mb=N] [--row-bytes=N]
//   --size-mb   target WAL file size in megabytes (default 32; this
//               machine has ~127GB free disk and 16GB RAM, so pass
//               something like --size-mb=1024 or 5120 for a 1GB/5GB run —
//               see BENCHMARKS.md for how those numbers scale to a
//               200GB/32-thread reference-hardware projection).
//   --row-bytes approximate on-disk WAL bytes per row, via a fixed-length
//               string payload (default 256).
//
// The workload size is *never* hard-coded to a target duration: this
// program measures whatever the actual replay takes and prints that.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "bench_common.h"
#include "coredb/db/database.h"
#include "coredb/recovery/recovery_manager.h"

using namespace coredb;
using namespace coredb::storage;

namespace {

Schema Events() { return {{"payload", ColumnType::kString}}; }

struct Args {
  uint64_t size_mb = 32;
  size_t row_bytes = 256;
};

Args ParseArgs(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a.rfind("--size-mb=", 0) == 0) args.size_mb = std::strtoull(a.c_str() + 10, nullptr, 10);
    if (a.rfind("--row-bytes=", 0) == 0) args.row_bytes = std::strtoull(a.c_str() + 12, nullptr, 10);
  }
  return args;
}

// Writes rows until the WAL file reaches target_bytes, checking real file
// size periodically rather than estimating serialization overhead by hand.
uint64_t GenerateWal(const std::string& dir, uint64_t target_bytes, size_t row_bytes) {
  db::Database d(dir, Events(), wal::WalWriterConfig{wal::DurabilityMode::kNoSync, 1});
  const std::string payload(row_bytes, 'x');
  uint64_t rows_written = 0;
  constexpr uint64_t kCheckEvery = 2000;

  while (true) {
    auto txn = d.Begin();
    for (uint64_t i = 0; i < kCheckEvery; ++i) {
      d.Insert(txn, {payload});
      ++rows_written;
    }
    d.Commit(txn);
    if (std::filesystem::file_size(d.wal_path()) >= target_bytes) break;
  }
  return rows_written;
}

}  // namespace

int main(int argc, char** argv) {
  bench::PrintBanner("bench_recovery");
  const Args args = ParseArgs(argc, argv);

  const std::string dir = (std::filesystem::temp_directory_path() / "coredb_bench_recovery").string();
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  const uint64_t target_bytes = args.size_mb * 1024ull * 1024ull;
  std::fprintf(stdout, "generating WAL: target=%llu MB, row_bytes=%zu ...\n",
               static_cast<unsigned long long>(args.size_mb), args.row_bytes);
  const auto gen_start = std::chrono::steady_clock::now();
  const uint64_t rows_written = GenerateWal(dir, target_bytes, args.row_bytes);
  const double gen_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - gen_start).count();

  const std::string wal_path = (std::filesystem::path(dir) / "wal.log").string();
  const uint64_t actual_bytes = std::filesystem::file_size(wal_path);
  std::fprintf(stdout, "generated %llu rows, %.1f MB actual, in %.2fs\n",
               static_cast<unsigned long long>(rows_written), actual_bytes / (1024.0 * 1024.0), gen_seconds);

  auto jsonl = bench::OpenResultsFile("bench_recovery");
  const std::vector<unsigned> worker_counts = {1, 2, 4, 8, 16, 32};
  double single_thread_seconds = 0.0;

  for (unsigned workers : worker_counts) {
    table::Table table("events", Events());
    recovery::RecoveryConfig config;
    config.wal_path = wal_path;
    config.checkpoint_manifest_path = (std::filesystem::path(dir) / "checkpoint" / "manifest.txt").string();
    config.segment_dir = (std::filesystem::path(dir) / "checkpoint").string();
    config.num_workers = workers;

    const recovery::RecoveryStats stats = recovery::RecoveryManager::Recover(table, config);
    const double seconds = std::chrono::duration<double>(stats.duration).count();
    if (workers == 1) single_thread_seconds = seconds;
    const double speedup = single_thread_seconds > 0 ? single_thread_seconds / seconds : 1.0;
    const double mb_per_sec = seconds > 0 ? (actual_bytes / (1024.0 * 1024.0)) / seconds : 0.0;
    const double rows_per_sec = seconds > 0 ? static_cast<double>(stats.operations_replayed) / seconds : 0.0;

    std::fprintf(stdout,
                  "workers=%2u duration_s=%8.4f MB/s=%9.1f rows/s=%12.0f speedup_vs_1thread=%5.2fx "
                  "ops_replayed=%llu checksum_ok=%s\n",
                  workers, seconds, mb_per_sec, rows_per_sec, speedup,
                  static_cast<unsigned long long>(stats.operations_replayed),
                  stats.checksum_verification_passed ? "yes" : "NO");
    if (!stats.error.empty()) std::fprintf(stderr, "  error: %s\n", stats.error.c_str());

    jsonl << "{\"workers\":" << workers << ",\"duration_s\":" << seconds << ",\"mb_per_sec\":" << mb_per_sec
          << ",\"rows_per_sec\":" << rows_per_sec << ",\"speedup_vs_1thread\":" << speedup
          << ",\"ops_replayed\":" << stats.operations_replayed
          << ",\"checksum_ok\":" << (stats.checksum_verification_passed ? "true" : "false")
          << ",\"wal_bytes\":" << actual_bytes << "}\n";
  }

  std::filesystem::remove_all(dir);
  return 0;
}
