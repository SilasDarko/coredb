#include "coredb/table/table.h"

#include <algorithm>

#include <gtest/gtest.h>

using namespace coredb::table;
using namespace coredb::storage;
using coredb::mvcc::Transaction;

namespace {

Schema Users() {
  return {{"name", ColumnType::kString}, {"age", ColumnType::kInt32}};
}

std::vector<Row> Visible(Table& table, const Transaction& reader) {
  return table.MaterializeVisibleRows(table.SnapshotForScan(reader));
}

}  // namespace

TEST(TableCrud, InsertIsVisibleAfterCommit) {
  Table table("users", Users());
  Transaction writer = table.Begin();
  const uint64_t id = table.Insert(writer, {std::string("alice"), int32_t{30}});
  table.Commit(writer);

  Transaction reader = table.Begin();
  auto rows = Visible(table, reader);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(std::get<std::string>(rows[0][0]), "alice");
  EXPECT_EQ(std::get<int32_t>(rows[0][1]), 30);
  (void)id;
}

TEST(TableCrud, UninsertedRowIsNotVisibleBeforeCommit) {
  Table table("users", Users());
  Transaction writer = table.Begin();
  table.Insert(writer, {std::string("bob"), int32_t{22}});

  Transaction concurrent_reader = table.Begin();
  EXPECT_TRUE(Visible(table, concurrent_reader).empty());

  table.Commit(writer);
  Transaction later_reader = table.Begin();
  EXPECT_EQ(Visible(table, later_reader).size(), 1u);
}

TEST(TableCrud, UpdateChangesValueForFutureReadersOnly) {
  Table table("users", Users());
  Transaction w1 = table.Begin();
  const uint64_t id = table.Insert(w1, {std::string("carol"), int32_t{40}});
  table.Commit(w1);

  Transaction old_reader = table.Begin();

  Transaction w2 = table.Begin();
  ASSERT_TRUE(table.Update(w2, id, {std::string("carol"), int32_t{41}}));
  table.Commit(w2);

  Transaction new_reader = table.Begin();

  auto old_rows = Visible(table, old_reader);
  ASSERT_EQ(old_rows.size(), 1u);
  EXPECT_EQ(std::get<int32_t>(old_rows[0][1]), 40);

  auto new_rows = Visible(table, new_reader);
  ASSERT_EQ(new_rows.size(), 1u);
  EXPECT_EQ(std::get<int32_t>(new_rows[0][1]), 41);
}

TEST(TableCrud, DeleteRemovesRowForFutureReadersOnly) {
  Table table("users", Users());
  Transaction w1 = table.Begin();
  const uint64_t id = table.Insert(w1, {std::string("dave"), int32_t{50}});
  table.Commit(w1);

  Transaction old_reader = table.Begin();

  Transaction w2 = table.Begin();
  ASSERT_TRUE(table.Delete(w2, id));
  table.Commit(w2);

  Transaction new_reader = table.Begin();
  EXPECT_EQ(Visible(table, old_reader).size(), 1u);
  EXPECT_EQ(Visible(table, new_reader).size(), 0u);
}

TEST(TableCrud, UpdateOfNonexistentRowFails) {
  Table table("users", Users());
  Transaction w = table.Begin();
  EXPECT_FALSE(table.Update(w, /*row_id=*/12345, {std::string("nobody"), int32_t{0}}));
  EXPECT_FALSE(table.Delete(w, 12345));
}

TEST(TableCrud, ConcurrentUpdatesToSameRowConflict) {
  Table table("users", Users());
  Transaction w1 = table.Begin();
  const uint64_t id = table.Insert(w1, {std::string("erin"), int32_t{20}});
  table.Commit(w1);

  Transaction a = table.Begin();
  Transaction b = table.Begin();
  ASSERT_TRUE(table.Update(a, id, {std::string("erin"), int32_t{21}}));
  EXPECT_FALSE(table.Update(b, id, {std::string("erin"), int32_t{22}}))
      << "a still-active concurrent writer must cause the second update to be rejected";
  table.Commit(a);
  table.Abort(b);
}

TEST(TableCrud, AbortedInsertNeverBecomesVisible) {
  Table table("users", Users());
  Transaction w = table.Begin();
  table.Insert(w, {std::string("ghost"), int32_t{0}});
  table.Abort(w);

  Transaction reader = table.Begin();
  EXPECT_TRUE(Visible(table, reader).empty());
}
