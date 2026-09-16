#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "coredb/wal/wal_reader.h"
#include "coredb/wal/wal_writer.h"

using namespace coredb::wal;

namespace {

std::string TempWalPath(const char* name) {
  return (std::filesystem::temp_directory_path() / name).string();
}

}  // namespace

TEST(WalTruncation, CleanLogReadsBackExactly) {
  const std::string path = TempWalPath("coredb_wal_clean.log");
  std::filesystem::remove(path);
  {
    WalWriter writer(path, WalWriterConfig{DurabilityMode::kNoSync, 1});
    writer.Append(1, WalRecordType::kBegin);
    writer.Append(1, WalRecordType::kInsert, 100, {std::string("hello")});
    writer.Append(1, WalRecordType::kCommit);
  }
  WalReadResult result = ReadAll(path);
  EXPECT_FALSE(result.truncated_tail);
  EXPECT_FALSE(result.corrupt_record);
  EXPECT_EQ(result.records.size(), 3u);
  std::filesystem::remove(path);
}

TEST(WalTruncation, PartialFinalRecordIsDetectedAndPriorRecordsSurvive) {
  const std::string path = TempWalPath("coredb_wal_truncated.log");
  std::filesystem::remove(path);
  {
    WalWriter writer(path, WalWriterConfig{DurabilityMode::kNoSync, 1});
    writer.Append(1, WalRecordType::kBegin);
    writer.Append(1, WalRecordType::kInsert, 1, {std::string("first")});
    writer.Append(1, WalRecordType::kCommit);
    writer.Append(2, WalRecordType::kBegin);
    writer.Append(2, WalRecordType::kInsert, 2, {std::string("second-will-be-cut")});
  }

  // Simulate a crash mid-write: chop the last N bytes off, as if the
  // process died partway through fwrite()-ing the final record.
  const auto full_size = std::filesystem::file_size(path);
  std::filesystem::resize_file(path, full_size - 5);

  WalReadResult result = ReadAll(path);
  EXPECT_TRUE(result.truncated_tail);
  EXPECT_FALSE(result.corrupt_record);
  ASSERT_EQ(result.records.size(), 4u) << "the 3 complete records plus txn 2's BEGIN must survive";
  EXPECT_EQ(result.records[0].type, WalRecordType::kBegin);
  EXPECT_EQ(result.records[2].type, WalRecordType::kCommit);
  EXPECT_EQ(result.records[3].type, WalRecordType::kBegin);
  std::filesystem::remove(path);
}

TEST(WalTruncation, EmptyFileIsNotTreatedAsTruncated) {
  const std::string path = TempWalPath("coredb_wal_empty.log");
  std::filesystem::remove(path);
  { std::ofstream touch(path); }
  WalReadResult result = ReadAll(path);
  EXPECT_FALSE(result.truncated_tail);
  EXPECT_FALSE(result.corrupt_record);
  EXPECT_TRUE(result.records.empty());
  std::filesystem::remove(path);
}
