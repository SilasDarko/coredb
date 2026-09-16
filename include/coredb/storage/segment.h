#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "coredb/storage/dictionary.h"
#include "coredb/storage/types.h"
#include "coredb/util/bitmap.h"

namespace coredb::storage {

// One column's worth of data inside a Segment. Exactly one of the typed
// arrays below is populated, selected by `type`; the others stay empty.
// Kept as explicit typed members (rather than a std::variant<vector<...>>)
// so the SIMD/JIT code paths can take a raw `const int32_t*` straight out of
// the column with no branching or unwrapping.
class Column {
 public:
  ColumnType type() const { return type_; }
  size_t num_rows() const { return validity_.size(); }
  const util::Bitmap& validity() const { return validity_; }
  const Value& min() const { return min_; }
  const Value& max() const { return max_; }
  uint32_t checksum() const { return checksum_; }

  const int32_t* AsInt32() const { return int32_data_.data(); }
  const int64_t* AsInt64() const { return int64_data_.data(); }
  const double* AsDouble() const { return double_data_.data(); }
  const uint32_t* AsStringCodes() const { return string_codes_.data(); }
  const Dictionary& dictionary() const { return dictionary_; }

  Value ValueAt(size_t row) const;

 private:
  friend class Segment;
  friend class SegmentBuilder;

  ColumnType type_ = ColumnType::kInt32;
  util::Bitmap validity_;
  Value min_;
  Value max_;
  uint32_t checksum_ = 0;

  std::vector<int32_t> int32_data_;
  std::vector<int64_t> int64_data_;
  std::vector<double> double_data_;
  std::vector<uint32_t> string_codes_;
  Dictionary dictionary_;
};

// An immutable, columnar, on-disk-representable block of rows. Segments are
// never mutated after Build()/LoadFromFile(): updates and deletes are
// absorbed by the MVCC delta layer (see mvcc/delta.h) and only become part
// of a new Segment when the compactor merges them in.
class Segment {
 public:
  size_t num_rows() const { return num_rows_; }
  uint64_t segment_id() const { return segment_id_; }
  const Schema& schema() const { return schema_; }
  const Column& column(size_t index) const { return columns_.at(index); }
  size_t num_columns() const { return columns_.size(); }

  // Segment pruning: true if every row in this segment is guaranteed to
  // fail `column[col_idx] OP literal`, based solely on the column's
  // stored min/max. False negatives are fine (caller falls back to scanning
  // the segment); false positives would silently drop matching rows, so
  // this is conservative — it returns false whenever it cannot prove skip
  // is safe (no min/max recorded, e.g. an all-null column).
  bool CanSkip(size_t col_idx, PredicateOp op, const Value& literal) const;

  // Recomputes every column's checksum from its in-memory bytes and
  // compares against the checksum recorded at Build()/LoadFromFile() time.
  // Used as an explicit post-recovery integrity pass, independent of the
  // checksum check LoadFromFile already performs while parsing the file.
  bool VerifyIntegrity(std::string* error) const;

  bool SaveToFile(const std::string& path, std::string* error) const;
  static std::optional<Segment> LoadFromFile(const std::string& path, const Schema& schema,
                                              std::string* error);

  // Builds an immutable segment from row-oriented data (used by initial
  // bulk load and by the compactor when it merges the delta layer into a
  // new base segment).
  static Segment Build(uint64_t segment_id, const Schema& schema, const std::vector<Row>& rows);

 private:
  uint64_t segment_id_ = 0;
  size_t num_rows_ = 0;
  Schema schema_;
  std::vector<Column> columns_;
};

}  // namespace coredb::storage
