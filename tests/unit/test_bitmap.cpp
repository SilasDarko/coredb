#include "coredb/util/bitmap.h"

#include <gtest/gtest.h>

using coredb::util::Bitmap;

TEST(Bitmap, DefaultAllValid) {
  Bitmap bm(10, /*initial_value=*/true);
  EXPECT_TRUE(bm.AllValid());
  for (size_t i = 0; i < 10; ++i) EXPECT_TRUE(bm.IsValid(i));
}

TEST(Bitmap, SetInvalidatesSingleBit) {
  Bitmap bm(17, true);  // spans 3 bytes, exercises the trailing-bit mask
  bm.SetValid(9, false);
  EXPECT_FALSE(bm.AllValid());
  for (size_t i = 0; i < 17; ++i) {
    EXPECT_EQ(bm.IsValid(i), i != 9) << "row " << i;
  }
  EXPECT_EQ(bm.CountValid(), 16u);
}

TEST(Bitmap, TrailingBitsClearedOnConstruction) {
  Bitmap bm(3, true);  // 3 bits used out of 8 in the byte
  EXPECT_EQ(bm.byte_size(), 1u);
  EXPECT_TRUE(bm.AllValid());
  const uint8_t byte = bm.data()[0];
  EXPECT_EQ(byte & 0xF8, 0) << "bits 3..7 must stay clear even though initial_value=true";
}

TEST(Bitmap, RoundTripsThroughFromBytes) {
  Bitmap bm(20, true);
  bm.SetValid(0, false);
  bm.SetValid(19, false);
  Bitmap restored = Bitmap::FromBytes(20, bm.raw_bytes());
  EXPECT_EQ(restored.CountValid(), bm.CountValid());
  for (size_t i = 0; i < 20; ++i) EXPECT_EQ(restored.IsValid(i), bm.IsValid(i));
}
