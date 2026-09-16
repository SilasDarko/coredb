#include <atomic>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "coredb/table/table.h"

using namespace coredb::table;
using namespace coredb::storage;
using coredb::mvcc::Transaction;

namespace {
Schema Counters() { return {{"thread", ColumnType::kInt32}, {"seq", ColumnType::kInt32}}; }
}  // namespace

TEST(ConcurrentTxns, ManyThreadsInsertingConcurrentlyAllSucceedAndAreVisible) {
  Table table("counters", Counters());
  constexpr int kThreads = 8;
  constexpr int kInsertsPerThread = 200;

  std::vector<std::thread> workers;
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&table, t] {
      for (int i = 0; i < kInsertsPerThread; ++i) {
        Transaction txn = table.Begin();
        table.Insert(txn, {int32_t{t}, int32_t{i}});
        table.Commit(txn);
      }
    });
  }
  for (auto& w : workers) w.join();

  Transaction reader = table.Begin();
  auto rows = table.MaterializeVisibleRows(table.SnapshotForScan(reader));
  EXPECT_EQ(rows.size(), static_cast<size_t>(kThreads * kInsertsPerThread));
}

TEST(ConcurrentTxns, ReaderSnapshotIsStableWhileWritersCommitConcurrently) {
  Table table("counters", Counters());
  Transaction seed = table.Begin();
  table.Insert(seed, {int32_t{0}, int32_t{0}});
  table.Commit(seed);

  Transaction long_reader = table.Begin();
  const size_t baseline = table.MaterializeVisibleRows(table.SnapshotForScan(long_reader)).size();

  // A bounded number of commits (not a free-spinning writer) is enough to
  // prove the property and keeps this fast under heavy instrumentation
  // (ThreadSanitizer, ~50-100x slowdown) instead of burning wall-clock time
  // proportional to however many inserts a tight spin loop manages to fit
  // into an arbitrary duration.
  constexpr int kWriterCommits = 300;
  std::atomic<int> writer_progress{0};
  std::thread writer([&] {
    for (int i = 0; i < kWriterCommits; ++i) {
      Transaction txn = table.Begin();
      table.Insert(txn, {int32_t{1}, int32_t{1}});
      table.Commit(txn);
      writer_progress.store(i + 1, std::memory_order_relaxed);
    }
  });
  for (int i = 0; i < 20; ++i) {
    // Repeated reads through the SAME snapshot must always see the same
    // count, no matter how many commits race ahead of it concurrently.
    EXPECT_EQ(table.MaterializeVisibleRows(table.SnapshotForScan(long_reader)).size(), baseline);
    std::this_thread::yield();
  }
  writer.join();
  EXPECT_EQ(writer_progress.load(), kWriterCommits);
  table.Abort(long_reader);
}

TEST(ConcurrentTxns, InterleavedCompactionUnderConcurrentWritersStaysConsistent) {
  Table table("counters", Counters());
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    int i = 0;
    while (!stop.load()) {
      Transaction txn = table.Begin();
      table.Insert(txn, {int32_t{0}, int32_t{i++}});
      table.Commit(txn);
    }
  });

  for (int i = 0; i < 20; ++i) {
    table.Compact();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  stop.store(true);
  writer.join();
  table.Compact();

  Transaction reader = table.Begin();
  const size_t total = table.MaterializeVisibleRows(table.SnapshotForScan(reader)).size();
  EXPECT_EQ(total, table.total_base_rows() + table.delta_size());
  EXPECT_GT(total, 0u);
}
