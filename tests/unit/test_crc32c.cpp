#include "coredb/util/crc32c.h"

#include <gtest/gtest.h>

using coredb::util::Crc32c;
using coredb::util::Crc32cExtend;

TEST(Crc32c, KnownVector) {
  // Standard CRC-32C check value for the ASCII string "123456789".
  const char data[] = "123456789";
  EXPECT_EQ(Crc32c(data, 9), 0xE3069283u);
}

TEST(Crc32c, EmptyInputIsZero) { EXPECT_EQ(Crc32c(nullptr, 0), 0u); }

TEST(Crc32c, DifferentBytesDifferentChecksum) {
  const char a[] = "coredb-segment-block";
  const char b[] = "coredb-segment-Block";  // one bit flip's worth of difference
  EXPECT_NE(Crc32c(a, sizeof(a) - 1), Crc32c(b, sizeof(b) - 1));
}

TEST(Crc32c, ExtendIsEquivalentToWholeBuffer) {
  const char data[] = "the quick brown fox jumps over the lazy dog";
  const size_t len = sizeof(data) - 1;
  const uint32_t whole = Crc32c(data, len);

  const size_t split = 17;
  uint32_t incremental = Crc32cExtend(0, data, split);
  incremental = Crc32cExtend(incremental, data + split, len - split);
  EXPECT_EQ(whole, incremental);
}
