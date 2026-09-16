// Compaction throughput and pause time under churn: repeatedly insert a
// batch, delete a fraction of previously-inserted rows, and run
// Table::Compact(), measuring how long each compaction pass takes as the
// base segment and delta layer grow. Reports rows/sec compacted and the
// metrics Table itself tracks (promoted/retired counts).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

#include "bench_common.h"
#include "coredb/table/table.h"

using namespace coredb;
using namespace coredb::storage;
using coredb::mvcc::Transaction;

namespace {
Schema Events() { return {{"payload", ColumnType::kInt64}}; }
}  // namespace

int main() {
  bench::PrintBanner("bench_compaction");
  auto jsonl = bench::OpenResultsFile("bench_compaction");

  const std::vector<size_t> batch_sizes = {1000, 10000, 50000};
  const int kRounds = 5;

  for (size_t batch_size : batch_sizes) {
    table::Table table("events", Events());
    std::vector<uint64_t> live_ids;
    std::mt19937_64 rng(batch_size);

    for (int round = 0; round < kRounds; ++round) {
      Transaction insert_txn = table.Begin();
      for (size_t i = 0; i < batch_size; ++i) {
        live_ids.push_back(table.Insert(insert_txn, {int64_t{static_cast<int64_t>(round * batch_size + i)}}));
      }
      table.Commit(insert_txn);

      // Delete ~10% of everything inserted so far, to exercise both
      // promotion (new rows) and purge-on-rewrite (tombstoned rows) paths.
      const size_t delete_count = live_ids.size() / 10;
      if (delete_count > 0) {
        Transaction delete_txn = table.Begin();
        std::shuffle(live_ids.begin(), live_ids.end(), rng);
        for (size_t i = 0; i < delete_count; ++i) table.Delete(delete_txn, live_ids[i]);
        table.Commit(delete_txn);
        live_ids.erase(live_ids.begin(), live_ids.begin() + static_cast<long>(delete_count));
      }

      const size_t delta_before = table.delta_size();
      const auto start = std::chrono::steady_clock::now();
      table.Compact();
      const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      const auto metrics = table.compaction_metrics();
      const double rows_per_sec = seconds > 0 ? static_cast<double>(delta_before) / seconds : 0.0;

      std::fprintf(stdout,
                    "batch_size=%7zu round=%d delta_before=%7zu base_rows_after=%8zu "
                    "duration_ms=%8.3f rows_per_sec=%12.0f promoted=%llu retired=%llu\n",
                    batch_size, round, delta_before, table.total_base_rows(), seconds * 1000.0, rows_per_sec,
                    static_cast<unsigned long long>(metrics.rows_promoted),
                    static_cast<unsigned long long>(metrics.rows_retired));
      jsonl << "{\"batch_size\":" << batch_size << ",\"round\":" << round << ",\"delta_before\":" << delta_before
            << ",\"base_rows_after\":" << table.total_base_rows() << ",\"duration_s\":" << seconds
            << ",\"rows_per_sec\":" << rows_per_sec << ",\"rows_promoted\":" << metrics.rows_promoted
            << ",\"rows_retired\":" << metrics.rows_retired << "}\n";
    }
  }
  return 0;
}
