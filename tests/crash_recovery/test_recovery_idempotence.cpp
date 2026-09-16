#include <algorithm>
#include <filesystem>

#include <gtest/gtest.h>

#include "coredb/db/database.h"

using namespace coredb;
using namespace coredb::storage;

namespace {

Schema Accounts() { return {{"owner", ColumnType::kString}, {"balance", ColumnType::kInt64}}; }

std::string FreshDir(const char* name) {
  const auto dir = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove_all(dir);
  return dir.string();
}

std::vector<Row> AllVisible(db::Database& d) {
  auto reader = d.Begin();
  auto rows = d.table().MaterializeVisibleRows(d.table().SnapshotForScan(reader));
  d.Abort(reader);  // read-only; discard cleanly rather than leaving it dangling active
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    return std::get<std::string>(a[0]) < std::get<std::string>(b[0]);
  });
  return rows;
}

}  // namespace

TEST(RecoveryIdempotence, ReplayReconstructsCommittedStateAfterSimulatedCrash) {
  const std::string dir = FreshDir("coredb_recovery_idem_1");

  {
    db::Database d(dir, Accounts());
    auto t1 = d.Begin();
    const uint64_t alice = d.Insert(t1, {std::string("alice"), int64_t{100}});
    d.Insert(t1, {std::string("bob"), int64_t{50}});
    d.Commit(t1);

    auto t2 = d.Begin();
    d.Update(t2, alice, {std::string("alice"), int64_t{80}});
    d.Commit(t2);

    auto t3 = d.Begin();  // never committed: must NOT survive recovery
    d.Insert(t3, {std::string("ghost"), int64_t{999}});
    // deliberately no Commit/Abort — simulates the process dying here
  }
  // `d` (and its WalWriter) is destroyed above without an explicit
  // checkpoint, exactly like an unclean shutdown: everything durability
  // depends on must come from the WAL alone.

  db::Database recovered(dir, Accounts());
  auto rows = AllVisible(recovered);
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(std::get<std::string>(rows[0][0]), "alice");
  EXPECT_EQ(std::get<int64_t>(rows[0][1]), 80);
  EXPECT_EQ(std::get<std::string>(rows[1][0]), "bob");
  EXPECT_EQ(std::get<int64_t>(rows[1][1]), 50);

  std::filesystem::remove_all(dir);
}

TEST(RecoveryIdempotence, RecoveringTwiceInARowYieldsIdenticalState) {
  const std::string dir = FreshDir("coredb_recovery_idem_2");
  {
    db::Database d(dir, Accounts());
    auto t = d.Begin();
    d.Insert(t, {std::string("carol"), int64_t{10}});
    d.Commit(t);
  }

  std::vector<Row> first_pass;
  {
    db::Database d(dir, Accounts());
    first_pass = AllVisible(d);
  }
  std::vector<Row> second_pass;
  {
    db::Database d(dir, Accounts());
    second_pass = AllVisible(d);
  }

  ASSERT_EQ(first_pass.size(), second_pass.size());
  for (size_t i = 0; i < first_pass.size(); ++i) {
    EXPECT_EQ(first_pass[i], second_pass[i]);
  }
  std::filesystem::remove_all(dir);
}

TEST(RecoveryIdempotence, CheckpointThenReplayOfTailProducesSameResultAsFullReplay) {
  const std::string dir_full = FreshDir("coredb_recovery_full");
  const std::string dir_ckpt = FreshDir("coredb_recovery_ckpt");

  auto build = [](const std::string& dir, bool checkpoint_midway) {
    db::Database d(dir, Accounts());
    auto t1 = d.Begin();
    const uint64_t id = d.Insert(t1, {std::string("dana"), int64_t{5}});
    d.Commit(t1);
    if (checkpoint_midway) {
      std::string error;
      ASSERT_TRUE(d.Checkpoint(&error)) << error;
    }
    auto t2 = d.Begin();
    d.Update(t2, id, {std::string("dana"), int64_t{15}});
    d.Commit(t2);
  };
  build(dir_full, /*checkpoint_midway=*/false);
  build(dir_ckpt, /*checkpoint_midway=*/true);

  db::Database recovered_full(dir_full, Accounts());
  db::Database recovered_ckpt(dir_ckpt, Accounts());
  EXPECT_TRUE(recovered_ckpt.recovery_stats().checkpoint_found);
  EXPECT_FALSE(recovered_full.recovery_stats().checkpoint_found);

  auto rows_full = AllVisible(recovered_full);
  auto rows_ckpt = AllVisible(recovered_ckpt);
  ASSERT_EQ(rows_full.size(), rows_ckpt.size());
  for (size_t i = 0; i < rows_full.size(); ++i) EXPECT_EQ(rows_full[i], rows_ckpt[i]);

  std::filesystem::remove_all(dir_full);
  std::filesystem::remove_all(dir_ckpt);
}
