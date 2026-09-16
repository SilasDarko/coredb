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

TEST(WalCorruption, FlippedByteInMiddleRecordIsDetected) {
  const std::string path = TempWalPath("coredb_wal_corrupt.log");
  std::filesystem::remove(path);
  uint64_t second_record_offset = 0;
  {
    WalWriter writer(path, WalWriterConfig{DurabilityMode::kNoSync, 1});
    writer.Append(1, WalRecordType::kBegin);
    second_record_offset = std::filesystem::file_size(path);
    writer.Append(1, WalRecordType::kInsert, 1, {std::string("payload-to-corrupt")});
    writer.Append(1, WalRecordType::kCommit);
  }

  // Flip a byte inside the second record's body (well past its own length
  // and checksum header) so the checksum stored for it no longer matches.
  {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(f);
    const auto offset = static_cast<std::streamoff>(second_record_offset + 20);
    f.seekg(offset);
    char c;
    f.read(&c, 1);
    c ^= 0xFF;
    f.seekp(offset);
    f.write(&c, 1);
  }

  WalReadResult result = ReadAll(path);
  EXPECT_TRUE(result.corrupt_record);
  EXPECT_FALSE(result.truncated_tail);
  ASSERT_EQ(result.records.size(), 1u) << "only the BEGIN record before the corruption should survive";
  EXPECT_EQ(result.records[0].type, WalRecordType::kBegin);
  std::filesystem::remove(path);
}

TEST(WalCorruption, CorruptionInLengthHeaderStopsAtPriorRecords) {
  const std::string path = TempWalPath("coredb_wal_corrupt_len.log");
  std::filesystem::remove(path);
  uint64_t second_record_offset = 0;
  {
    WalWriter writer(path, WalWriterConfig{DurabilityMode::kNoSync, 1});
    writer.Append(1, WalRecordType::kBegin);
    second_record_offset = std::filesystem::file_size(path);
    writer.Append(1, WalRecordType::kCommit);
  }

  // Corrupt the length prefix itself to an absurdly large value; the reader
  // must treat "declared length longer than remaining file" as damage
  // rather than crashing or reading out of bounds.
  {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(f);
    uint32_t bogus_len = 0xFFFFFFF0u;
    f.seekp(static_cast<std::streamoff>(second_record_offset));
    f.write(reinterpret_cast<char*>(&bogus_len), sizeof(bogus_len));
  }

  WalReadResult result = ReadAll(path);
  EXPECT_TRUE(result.truncated_tail);
  ASSERT_EQ(result.records.size(), 1u);
  EXPECT_EQ(result.records[0].type, WalRecordType::kBegin);
  std::filesystem::remove(path);
}
