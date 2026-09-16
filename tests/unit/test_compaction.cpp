#include "coredb/table/table.h"

#include <gtest/gtest.h>

using namespace coredb::table;
using namespace coredb::storage;
using coredb::mvcc::Transaction;

namespace {

Schema Items() { return {{"label", ColumnType::kString}}; }

std::vector<Row> Visible(Table& table, const Transaction& reader) {
  return table.MaterializeVisibleRows(table.SnapshotForScan(reader));
}

}  // namespace

TEST(Compaction, PromotesCommittedRowsIntoBaseSegment) {
  Table table("items", Items());
  Transaction w = table.Begin();
  table.Insert(w, {std::string("a")});
  table.Insert(w, {std::string("b")});
  table.Commit(w);

  ASSERT_EQ(table.delta_size(), 2u);
  ASSERT_EQ(table.num_base_segments(), 0u);

  table.Compact();

  EXPECT_EQ(table.delta_size(), 0u) << "both rows were stably committed, so they should be promoted";
  EXPECT_EQ(table.num_base_segments(), 1u);
  EXPECT_EQ(table.total_base_rows(), 2u);

  Transaction reader = table.Begin();
  EXPECT_EQ(Visible(table, reader).size(), 2u);
}

TEST(Compaction, DoesNotPromoteRowsStillNeededByAnActiveSnapshot) {
  Table table("items", Items());
  Transaction blocker = table.Begin();  // snapshot predates the insert below

  Transaction w = table.Begin();
  table.Insert(w, {std::string("c")});
  table.Commit(w);

  table.Compact();

  // blocker's snapshot_ts is older than w's commit_ts, so the new row is not
  // yet "stable" relative to the oldest active snapshot: it must stay in
  // the delta layer rather than being promoted into an unconditionally
  // visible base segment (which would incorrectly expose it to blocker).
  EXPECT_EQ(table.num_base_segments(), 0u);
  EXPECT_EQ(table.delta_size(), 1u);
  EXPECT_TRUE(Visible(table, blocker).empty());

  table.Abort(blocker);
}

TEST(Compaction, PurgesStablyDeletedRowsFromBaseOnRewrite) {
  Table table("items", Items());
  Transaction w1 = table.Begin();
  const uint64_t id = table.Insert(w1, {std::string("d")});
  table.Commit(w1);
  table.Compact();
  ASSERT_EQ(table.total_base_rows(), 1u);

  Transaction w2 = table.Begin();
  ASSERT_TRUE(table.Delete(w2, id));
  table.Commit(w2);

  table.Compact();

  EXPECT_EQ(table.total_base_rows(), 0u);
  EXPECT_EQ(table.delta_size(), 0u);
  Transaction reader = table.Begin();
  EXPECT_TRUE(Visible(table, reader).empty());
}

TEST(Compaction, ActiveSnapshotStillSeesRowThroughACompactionCycle) {
  Table table("items", Items());
  Transaction w1 = table.Begin();
  const uint64_t id = table.Insert(w1, {std::string("e")});
  table.Commit(w1);
  table.Compact();  // promote into base

  Transaction long_reader = table.Begin();  // will outlive the delete below

  Transaction w2 = table.Begin();
  ASSERT_TRUE(table.Delete(w2, id));
  table.Commit(w2);

  table.Compact();  // must NOT purge the row: long_reader might still need it

  EXPECT_EQ(Visible(table, long_reader).size(), 1u)
      << "active-snapshot protection: compaction must not remove a version a running reader can still see";

  table.Abort(long_reader);
  Transaction new_reader = table.Begin();
  table.Compact();  // now safe to purge
  EXPECT_TRUE(Visible(table, new_reader).empty());
}

TEST(Compaction, RepeatedCompactionIsIdempotentWhenNothingChanged) {
  Table table("items", Items());
  Transaction w = table.Begin();
  table.Insert(w, {std::string("f")});
  table.Commit(w);
  table.Compact();
  const size_t rows_after_first = table.total_base_rows();
  table.Compact();
  table.Compact();
  EXPECT_EQ(table.total_base_rows(), rows_after_first);
}
