// MVCC delta-layer transaction throughput: N worker threads issuing
// single-row commits against a shared Table, measured across worker
// counts. Two workloads:
//   insert  — every txn inserts a brand-new row (no contention possible)
//   update  — every txn updates a row drawn from a small fixed key set
//             (high contention: many txns will lose the write-write race
//             and abort, which is reported too, not hidden)
//
// This benchmark is the source of truth for reported transaction-throughput measurements.
// see BENCHMARKS.md for what was actually measured on this machine and
// what workload/durability assumptions that number depends on.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

#include "bench_common.h"
#include "coredb/compaction/compactor.h"
#include "coredb/table/table.h"

using namespace coredb;
using namespace coredb::storage;
using coredb::mvcc::Transaction;

namespace {

Schema Ledger() { return {{"value", ColumnType::kInt64}}; }

struct RunResult {
  unsigned threads;
  uint64_t committed;
  uint64_t aborted;
  double seconds;
};

RunResult RunInsertWorkload(unsigned num_threads, std::chrono::milliseconds duration) {
  table::Table table("ledger", Ledger());
  // Deliberately no background compactor here: Insert() never scans the
  // delta layer looking for a row_id (that cost is specific to
  // Update/Delete's claim logic), so a pure-insert workload has nothing to
  // gain from compacting and everything to lose — CoreDB's compactor does
  // a full base-segment rewrite every cycle (see DESIGN_DECISIONS.md), and
  // running that on a never-shrinking insert-only base is pure overhead.
  // The update workload below, which DOES pay for an unbounded delta, runs
  // one.
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> committed{0};

  std::vector<std::thread> workers;
  const auto start = std::chrono::steady_clock::now();
  for (unsigned t = 0; t < num_threads; ++t) {
    workers.emplace_back([&] {
      uint64_t local = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        Transaction txn = table.Begin();
        table.Insert(txn, {static_cast<int64_t>(local)});
        table.Commit(txn);
        ++local;
      }
      committed.fetch_add(local, std::memory_order_relaxed);
    });
  }
  std::this_thread::sleep_for(duration);
  stop.store(true);
  for (auto& w : workers) w.join();
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  return {num_threads, committed.load(), 0, seconds};
}

RunResult RunUpdateContentionWorkload(unsigned num_threads, std::chrono::milliseconds duration,
                                       size_t key_space) {
  table::Table table("ledger", Ledger());
  compaction::BackgroundCompactor compactor(table, compaction::CompactorConfig{2000, std::chrono::milliseconds(5)});
  std::vector<uint64_t> row_ids(key_space);
  {
    Transaction seed = table.Begin();
    for (size_t i = 0; i < key_space; ++i) row_ids[i] = table.Insert(seed, {int64_t{0}});
    table.Commit(seed);
  }

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> committed{0};
  std::atomic<uint64_t> aborted{0};

  std::vector<std::thread> workers;
  const auto start = std::chrono::steady_clock::now();
  for (unsigned t = 0; t < num_threads; ++t) {
    workers.emplace_back([&, t] {
      std::mt19937_64 rng(t + 1);
      std::uniform_int_distribution<size_t> pick(0, key_space - 1);
      uint64_t local_commit = 0, local_abort = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        const uint64_t row_id = row_ids[pick(rng)];
        Transaction txn = table.Begin();
        if (table.Update(txn, row_id, {int64_t{1}})) {
          table.Commit(txn);
          ++local_commit;
        } else {
          table.Abort(txn);
          ++local_abort;
        }
      }
      committed.fetch_add(local_commit, std::memory_order_relaxed);
      aborted.fetch_add(local_abort, std::memory_order_relaxed);
    });
  }
  std::this_thread::sleep_for(duration);
  stop.store(true);
  for (auto& w : workers) w.join();
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  return {num_threads, committed.load(), aborted.load(), seconds};
}

void Report(const char* workload, const RunResult& r, std::ostream& jsonl) {
  const double txns_per_sec = static_cast<double>(r.committed) / r.seconds;
  std::fprintf(stdout, "workload=%-8s threads=%2u committed=%10llu aborted=%8llu duration_s=%.3f txns_per_sec=%.0f\n",
               workload, r.threads, static_cast<unsigned long long>(r.committed),
               static_cast<unsigned long long>(r.aborted), r.seconds, txns_per_sec);
  jsonl << "{\"workload\":\"" << workload << "\",\"threads\":" << r.threads << ",\"committed\":" << r.committed
        << ",\"aborted\":" << r.aborted << ",\"duration_s\":" << r.seconds
        << ",\"txns_per_sec\":" << txns_per_sec << "}\n";
}

}  // namespace

int main() {
  bench::PrintBanner("bench_mvcc_throughput");
  auto jsonl = bench::OpenResultsFile("bench_mvcc_throughput");

  const std::vector<unsigned> thread_counts = {1, 2, 4, 8};
  const auto duration = std::chrono::milliseconds(1500);

  std::fprintf(stdout, "-- insert workload (no contention; every txn creates a new row) --\n");
  for (unsigned t : thread_counts) Report("insert", RunInsertWorkload(t, duration), jsonl);

  std::fprintf(stdout, "-- update workload (16-key hotspot; write-write conflicts abort) --\n");
  for (unsigned t : thread_counts) Report("update", RunUpdateContentionWorkload(t, duration, /*key_space=*/16), jsonl);

  return 0;
}
