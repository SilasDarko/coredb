#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace coredb::util {

// A validity bitmap: one bit per row, 1 = non-null. Used by every column in
// a segment so scans can skip null-checks entirely for all-valid columns
// (the common case) via `bitmap.AllValid()`.
class Bitmap {
 public:
  Bitmap() = default;
  explicit Bitmap(size_t num_rows, bool initial_value = true)
      : num_rows_(num_rows), bytes_((num_rows + 7) / 8, initial_value ? 0xFF : 0x00) {
    ClearTrailingBits();
  }

  size_t size() const { return num_rows_; }
  const uint8_t* data() const { return bytes_.data(); }
  size_t byte_size() const { return bytes_.size(); }

  bool IsValid(size_t row) const { return (bytes_[row >> 3] >> (row & 7)) & 1u; }

  void SetValid(size_t row, bool valid) {
    uint8_t& byte = bytes_[row >> 3];
    const uint8_t mask = static_cast<uint8_t>(1u << (row & 7));
    if (valid) {
      byte |= mask;
    } else {
      byte &= static_cast<uint8_t>(~mask);
    }
  }

  // True if every row is valid. Cached lazily is tempting but the bitmap is
  // small (num_rows/8 bytes) and this is only called once per segment scan
  // setup, so a linear scan keeps the class simple to reason about.
  bool AllValid() const {
    for (size_t i = 0; i + 1 < bytes_.size(); ++i) {
      if (bytes_[i] != 0xFF) return false;
    }
    if (bytes_.empty()) return true;
    const uint8_t last_mask = LastByteMask();
    return (bytes_.back() & last_mask) == last_mask;
  }

  size_t CountValid() const {
    size_t count = 0;
    for (size_t row = 0; row < num_rows_; ++row) {
      count += IsValid(row) ? 1 : 0;
    }
    return count;
  }

  const std::vector<uint8_t>& raw_bytes() const { return bytes_; }

  static Bitmap FromBytes(size_t num_rows, std::vector<uint8_t> bytes) {
    Bitmap bm;
    bm.num_rows_ = num_rows;
    bm.bytes_ = std::move(bytes);
    return bm;
  }

 private:
  uint8_t LastByteMask() const {
    const size_t used_bits = num_rows_ % 8;
    return used_bits == 0 ? 0xFF : static_cast<uint8_t>((1u << used_bits) - 1);
  }

  void ClearTrailingBits() {
    if (!bytes_.empty()) {
      bytes_.back() &= LastByteMask();
    }
  }

  size_t num_rows_ = 0;
  std::vector<uint8_t> bytes_;
};

}  // namespace coredb::util
