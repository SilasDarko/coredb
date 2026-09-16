#include <atomic>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "coredb/table/table.h"

using namespace coredb::table;
using namespace coredb::storage;
using coredb::mvcc::Transaction;

namespace {
Schema Accounts() { return {{"balance", ColumnType::kInt32}}; }
}  // namespace

TEST(Aborts, RacingUpdatesToSameRowExactlyOneWins) {
  Table table("accounts", Accounts());
  Transaction seed = table.Begin();
  const uint64_t id = table.Insert(seed, {int32_t{0}});
  table.Commit(seed);

  constexpr int kRacers = 16;
  std::vector<Transaction> txns;
  txns.reserve(kRacers);
  for (int i = 0; i < kRacers; ++i) txns.push_back(table.Begin());

  std::atomic<int> successes{0};
  std::vector<std::thread> workers;
  std::vector<uint8_t> won(kRacers, 0);
  for (int i = 0; i < kRacers; ++i) {
    workers.emplace_back([&, i] {
      if (table.Update(txns[i], id, {int32_t{i + 1}})) {
        won[i] = 1;
        successes.fetch_add(1);
      }
    });
  }
  for (auto& w : workers) w.join();

  EXPECT_EQ(successes.load(), 1) << "exactly one concurrent writer may claim an unclaimed row";

  int winner = -1;
  for (int i = 0; i < kRacers; ++i) {
    if (won[i]) {
      winner = i;
      table.Commit(txns[i]);
    } else {
      table.Abort(txns[i]);
    }
  }
  ASSERT_NE(winner, -1);

  Transaction reader = table.Begin();
  auto rows = table.MaterializeVisibleRows(table.SnapshotForScan(reader));
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(std::get<int32_t>(rows[0][0]), winner + 1);
}

TEST(Aborts, AbortedUpdateLeavesOriginalValueIntact) {
  Table table("accounts", Accounts());
  Transaction seed = table.Begin();
  const uint64_t id = table.Insert(seed, {int32_t{42}});
  table.Commit(seed);

  Transaction w = table.Begin();
  ASSERT_TRUE(table.Update(w, id, {int32_t{999}}));
  table.Abort(w);

  Transaction reader = table.Begin();
  auto rows = table.MaterializeVisibleRows(table.SnapshotForScan(reader));
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(std::get<int32_t>(rows[0][0]), 42) << "an aborted update must not be visible to anyone";
}

TEST(Aborts, RowCanBeReclaimedAfterAnAbortedClaim) {
  Table table("accounts", Accounts());
  Transaction seed = table.Begin();
  const uint64_t id = table.Insert(seed, {int32_t{1}});
  table.Commit(seed);

  Transaction a = table.Begin();
  ASSERT_TRUE(table.Update(a, id, {int32_t{2}}));
  table.Abort(a);

  Transaction b = table.Begin();
  EXPECT_TRUE(table.Update(b, id, {int32_t{3}})) << "an aborted claim must not permanently block the row";
  table.Commit(b);

  Transaction reader = table.Begin();
  auto rows = table.MaterializeVisibleRows(table.SnapshotForScan(reader));
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(std::get<int32_t>(rows[0][0]), 3);
}

TEST(Aborts, ManyAbortedInsertsUnderConcurrencyNeverBecomeVisible) {
  Table table("accounts", Accounts());
  constexpr int kThreads = 8;
  std::vector<std::thread> workers;
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&] {
      for (int i = 0; i < 100; ++i) {
        Transaction txn = table.Begin();
        table.Insert(txn, {int32_t{-1}});
        table.Abort(txn);
      }
    });
  }
  for (auto& w : workers) w.join();

  Transaction reader = table.Begin();
  EXPECT_TRUE(table.MaterializeVisibleRows(table.SnapshotForScan(reader)).empty());
}
