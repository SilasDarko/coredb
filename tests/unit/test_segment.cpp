#include "coredb/storage/segment.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

using namespace coredb::storage;

namespace {

Schema TestSchema() {
  return {
      {"id", ColumnType::kInt64},
      {"score", ColumnType::kDouble},
      {"name", ColumnType::kString},
  };
}

std::vector<Row> TestRows() {
  return {
      {int64_t{1}, 3.5, std::string("alice")},
      {int64_t{2}, std::monostate{}, std::string("bob")},  // null score
      {int64_t{3}, -1.25, std::string("alice")},            // repeated string -> dictionary reuse
      {int64_t{4}, 100.0, std::string("carol")},
  };
}

}  // namespace

TEST(Segment, BuildComputesMinMaxAndNulls) {
  Segment seg = Segment::Build(/*segment_id=*/1, TestSchema(), TestRows());
  EXPECT_EQ(seg.num_rows(), 4u);

  const Column& score = seg.column(1);
  EXPECT_EQ(score.validity().CountValid(), 3u);
  EXPECT_FALSE(score.validity().IsValid(1));
  EXPECT_EQ(std::get<double>(score.min()), -1.25);
  EXPECT_EQ(std::get<double>(score.max()), 100.0);

  const Column& name = seg.column(2);
  EXPECT_EQ(name.dictionary().size(), 3u);  // alice, bob, carol
}

TEST(Segment, CanSkipPrunesOutOfRangeSegments) {
  Segment seg = Segment::Build(1, TestSchema(), TestRows());
  // score column range is [-1.25, 100.0]
  EXPECT_TRUE(seg.CanSkip(1, PredicateOp::kGt, 1000.0));
  EXPECT_TRUE(seg.CanSkip(1, PredicateOp::kLt, -50.0));
  EXPECT_FALSE(seg.CanSkip(1, PredicateOp::kGt, 0.0));   // overlaps the range
  EXPECT_FALSE(seg.CanSkip(1, PredicateOp::kEq, 3.5));
  EXPECT_TRUE(seg.CanSkip(1, PredicateOp::kEq, 9999.0));
}

TEST(Segment, SaveAndLoadRoundTrips) {
  Segment original = Segment::Build(7, TestSchema(), TestRows());
  const std::filesystem::path path = std::filesystem::temp_directory_path() / "coredb_test_segment.bin";

  std::string error;
  ASSERT_TRUE(original.SaveToFile(path.string(), &error)) << error;

  auto loaded = Segment::LoadFromFile(path.string(), TestSchema(), &error);
  ASSERT_TRUE(loaded.has_value()) << error;
  EXPECT_EQ(loaded->segment_id(), 7u);
  EXPECT_EQ(loaded->num_rows(), original.num_rows());

  for (size_t r = 0; r < original.num_rows(); ++r) {
    for (size_t c = 0; c < original.schema().size(); ++c) {
      EXPECT_EQ(loaded->column(c).ValueAt(r), original.column(c).ValueAt(r)) << "row " << r << " col " << c;
    }
  }
  std::filesystem::remove(path);
}

TEST(Segment, LoadDetectsCorruption) {
  Segment original = Segment::Build(3, TestSchema(), TestRows());
  const std::filesystem::path path = std::filesystem::temp_directory_path() / "coredb_test_segment_corrupt.bin";
  std::string error;
  ASSERT_TRUE(original.SaveToFile(path.string(), &error)) << error;

  // Flip the file's last byte. The last column written ("name", a string)
  // ends with its dictionary blob, which the checksum for that column
  // covers, so this reliably lands inside checksummed bytes regardless of
  // the exact on-disk layout.
  {
    const auto size = std::filesystem::file_size(path);
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(f);
    const auto offset = static_cast<std::streamoff>(size - 1);
    char c;
    f.seekg(offset);
    f.read(&c, 1);
    c ^= 0xFF;
    f.seekp(offset);
    f.write(&c, 1);
  }

  auto loaded = Segment::LoadFromFile(path.string(), TestSchema(), &error);
  EXPECT_FALSE(loaded.has_value());
  EXPECT_NE(error.find("corruption"), std::string::npos) << error;
  std::filesystem::remove(path);
}
