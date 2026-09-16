#include <algorithm>
#include <filesystem>
#include <unordered_map>

#include <gtest/gtest.h>

#include "coredb/db/database.h"

using namespace coredb;
using namespace coredb::storage;

namespace {

Schema Inventory() { return {{"sku", ColumnType::kString}, {"quantity", ColumnType::kInt32}}; }

std::string FreshDir(const char* name) {
  const auto dir = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove_all(dir);
  return dir.string();
}

std::unordered_map<std::string, int32_t> Snapshot(db::Database& d) {
  auto reader = d.Begin();
  auto rows = d.table().MaterializeVisibleRows(d.table().SnapshotForScan(reader));
  d.Abort(reader);
  std::unordered_map<std::string, int32_t> out;
  for (auto& r : rows) out[std::get<std::string>(r[0])] = std::get<int32_t>(r[1]);
  return out;
}

}  // namespace

TEST(EndToEnd, FullLifecycleInsertUpdateDeleteCompactCheckpointRecover) {
  const std::string dir = FreshDir("coredb_e2e_full");
  std::unordered_map<uint64_t, std::string> row_id_of_sku;

  {
    db::Database d(dir, Inventory());

    auto t1 = d.Begin();
    std::vector<uint64_t> ids;
    for (int i = 0; i < 500; ++i) {
      const std::string sku = "sku-" + std::to_string(i);
      const uint64_t id = d.Insert(t1, {sku, int32_t{100}});
      row_id_of_sku[id] = sku;
      ids.push_back(id);
    }
    d.Commit(t1);

    d.table().Compact();  // fold into a base segment

    auto t2 = d.Begin();
    for (int i = 0; i < 100; ++i) {
      d.Update(t2, ids[i], {row_id_of_sku[ids[i]], int32_t{100 - i}});
    }
    for (int i = 100; i < 150; ++i) {
      d.Delete(t2, ids[i]);
    }
    d.Commit(t2);

    std::string error;
    ASSERT_TRUE(d.Checkpoint(&error)) << error;

    auto t3 = d.Begin();
    d.Insert(t3, {std::string("sku-post-checkpoint"), int32_t{7}});
    d.Commit(t3);
  }

  db::Database recovered(dir, Inventory());
  auto state = Snapshot(recovered);

  EXPECT_EQ(state.size(), 500u - 50u + 1u);  // 500 inserted, 50 deleted, 1 after checkpoint
  EXPECT_EQ(state.at("sku-0"), 100);         // updated
  EXPECT_EQ(state.at("sku-99"), 1);          // updated (100 - 99)
  EXPECT_EQ(state.find("sku-105"), state.end());  // deleted
  EXPECT_EQ(state.at("sku-200"), 100);       // untouched
  EXPECT_EQ(state.at("sku-post-checkpoint"), 7);

  for (const auto& seg : recovered.table().base_segments()) {
    std::string error;
    EXPECT_TRUE(seg->VerifyIntegrity(&error)) << error;
  }

  std::filesystem::remove_all(dir);
}

TEST(EndToEnd, SegmentPruningSkipsOutOfRangeSegments) {
  Segment seg = Segment::Build(1, Inventory(),
                                {
                                    {std::string("a"), int32_t{10}},
                                    {std::string("b"), int32_t{20}},
                                });
  EXPECT_TRUE(seg.CanSkip(1, PredicateOp::kGt, 1000));
  EXPECT_FALSE(seg.CanSkip(1, PredicateOp::kGt, 15));
}
