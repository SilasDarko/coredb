#include "coredb/util/crc32c.h"

#include <array>

namespace coredb::util {

namespace {

// Reflected CRC-32C polynomial (Castagnoli), same constant used by iSCSI,
// ext4, and most CRC32C table generators.
constexpr uint32_t kPoly = 0x82F63B78u;

std::array<uint32_t, 256> BuildTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) ? (crc >> 1) ^ kPoly : (crc >> 1);
    }
    table[i] = crc;
  }
  return table;
}

const std::array<uint32_t, 256>& Table() {
  static const std::array<uint32_t, 256> table = BuildTable();
  return table;
}

}  // namespace

uint32_t Crc32cExtend(uint32_t partial_crc, const void* data, size_t length) {
  const auto& table = Table();
  uint32_t crc = ~partial_crc;
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < length; ++i) {
    crc = table[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
  }
  return ~crc;
}

uint32_t Crc32c(const void* data, size_t length) { return Crc32cExtend(0, data, length); }

}  // namespace coredb::util
