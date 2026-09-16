#include "coredb/storage/segment.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "coredb/util/crc32c.h"

namespace coredb::storage {

namespace {

constexpr char kMagic[8] = {'C', 'D', 'B', 'S', 'E', 'G', '1', '\0'};

template <typename T>
void WriteRaw(std::ofstream& out, const T& value) {
  out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
bool ReadRaw(std::ifstream& in, T* value) {
  in.read(reinterpret_cast<char*>(value), sizeof(T));
  return static_cast<bool>(in);
}

void WriteBytes(std::ofstream& out, const void* data, size_t len) {
  out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
}

bool ReadBytes(std::ifstream& in, void* data, size_t len) {
  in.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(len));
  return static_cast<bool>(in);
}

void WriteString(std::ofstream& out, const std::string& s) {
  const uint32_t len = static_cast<uint32_t>(s.size());
  WriteRaw(out, len);
  WriteBytes(out, s.data(), len);
}

bool ReadString(std::ifstream& in, std::string* out_str) {
  uint32_t len = 0;
  if (!ReadRaw(in, &len)) return false;
  out_str->resize(len);
  return ReadBytes(in, out_str->data(), len);
}

// Encodes a Value known to already be of `type` (monostate is only valid
// when the caller has already confirmed there is no min/max to write).
void WriteTypedValue(std::ofstream& out, ColumnType type, const Value& v) {
  switch (type) {
    case ColumnType::kInt32: WriteRaw(out, std::get<int32_t>(v)); break;
    case ColumnType::kInt64: WriteRaw(out, std::get<int64_t>(v)); break;
    case ColumnType::kDouble: WriteRaw(out, std::get<double>(v)); break;
    case ColumnType::kString: WriteString(out, std::get<std::string>(v)); break;
  }
}

bool ReadTypedValue(std::ifstream& in, ColumnType type, Value* out_value) {
  switch (type) {
    case ColumnType::kInt32: {
      int32_t v;
      if (!ReadRaw(in, &v)) return false;
      *out_value = v;
      return true;
    }
    case ColumnType::kInt64: {
      int64_t v;
      if (!ReadRaw(in, &v)) return false;
      *out_value = v;
      return true;
    }
    case ColumnType::kDouble: {
      double v;
      if (!ReadRaw(in, &v)) return false;
      *out_value = v;
      return true;
    }
    case ColumnType::kString: {
      std::string v;
      if (!ReadString(in, &v)) return false;
      *out_value = std::move(v);
      return true;
    }
  }
  return false;
}

size_t ElementWidth(ColumnType type) {
  switch (type) {
    case ColumnType::kInt32: return sizeof(int32_t);
    case ColumnType::kInt64: return sizeof(int64_t);
    case ColumnType::kDouble: return sizeof(double);
    case ColumnType::kString: return sizeof(uint32_t);  // dictionary code
  }
  return 0;
}

const void* DataPointer(const Column& col) {
  switch (col.type()) {
    case ColumnType::kInt32: return col.AsInt32();
    case ColumnType::kInt64: return col.AsInt64();
    case ColumnType::kDouble: return col.AsDouble();
    case ColumnType::kString: return col.AsStringCodes();
  }
  return nullptr;
}

// Checksum covers the validity bitmap + raw data block + (for strings) the
// serialized dictionary, i.e. everything SaveToFile persists for this
// column. Computed the same way at Build() time and at load-verification
// time, so any bit flip anywhere in the column's bytes is caught.
uint32_t ComputeColumnChecksum(const Column& col) {
  uint32_t crc = util::Crc32cExtend(0, col.validity().data(), col.validity().byte_size());
  crc = util::Crc32cExtend(crc, DataPointer(col), col.num_rows() * ElementWidth(col.type()));
  if (col.type() == ColumnType::kString) {
    for (const auto& s : col.dictionary().values()) {
      crc = util::Crc32cExtend(crc, s.data(), s.size());
    }
  }
  return crc;
}

}  // namespace

Value Column::ValueAt(size_t row) const {
  if (!validity_.IsValid(row)) return std::monostate{};
  switch (type_) {
    case ColumnType::kInt32: return int32_data_[row];
    case ColumnType::kInt64: return int64_data_[row];
    case ColumnType::kDouble: return double_data_[row];
    case ColumnType::kString: return dictionary_.ValueOf(string_codes_[row]);
  }
  return std::monostate{};
}

bool Segment::VerifyIntegrity(std::string* error) const {
  for (size_t c = 0; c < columns_.size(); ++c) {
    const uint32_t recomputed = ComputeColumnChecksum(columns_[c]);
    if (recomputed != columns_[c].checksum()) {
      *error = "segment " + std::to_string(segment_id_) + " column " + std::to_string(c) +
               " failed integrity check (expected " + std::to_string(columns_[c].checksum()) +
               ", got " + std::to_string(recomputed) + ")";
      return false;
    }
  }
  return true;
}

bool Segment::CanSkip(size_t col_idx, PredicateOp op, const Value& literal) const {
  const Column& col = columns_.at(col_idx);
  if (std::holds_alternative<std::monostate>(col.min()) ||
      std::holds_alternative<std::monostate>(col.max())) {
    return false;  // no valid rows recorded (e.g. all-null column); can't prove anything
  }
  // A segment can be skipped only for range-shaped predicates where the
  // literal falls entirely outside [min, max]. Equality can also be pruned
  // the same way; inequality (!=) can practically never be pruned by a
  // single range, so we conservatively never skip for it.
  const int cmp_min = CompareValues(literal, col.min());
  const int cmp_max = CompareValues(literal, col.max());
  switch (op) {
    case PredicateOp::kEq: return cmp_min < 0 || cmp_max > 0;
    case PredicateOp::kGt: return cmp_max >= 0;
    case PredicateOp::kGe: return cmp_max > 0;
    case PredicateOp::kLt: return cmp_min <= 0;
    case PredicateOp::kLe: return cmp_min < 0;
    case PredicateOp::kNe: return false;
  }
  return false;
}

Segment Segment::Build(uint64_t segment_id, const Schema& schema, const std::vector<Row>& rows) {
  Segment seg;
  seg.segment_id_ = segment_id;
  seg.schema_ = schema;
  seg.num_rows_ = rows.size();
  seg.columns_.resize(schema.size());

  for (size_t c = 0; c < schema.size(); ++c) {
    Column& col = seg.columns_[c];
    col.type_ = schema[c].type;
    col.validity_ = util::Bitmap(rows.size(), /*initial_value=*/true);
    switch (col.type_) {
      case ColumnType::kInt32: col.int32_data_.resize(rows.size()); break;
      case ColumnType::kInt64: col.int64_data_.resize(rows.size()); break;
      case ColumnType::kDouble: col.double_data_.resize(rows.size()); break;
      case ColumnType::kString: col.string_codes_.resize(rows.size()); break;
    }

    bool have_bounds = false;
    for (size_t r = 0; r < rows.size(); ++r) {
      const Value& v = rows[r].at(c);
      if (std::holds_alternative<std::monostate>(v)) {
        col.validity_.SetValid(r, false);
        continue;
      }
      Value stored_value;
      switch (col.type_) {
        case ColumnType::kInt32: col.int32_data_[r] = std::get<int32_t>(v); stored_value = col.int32_data_[r]; break;
        case ColumnType::kInt64: col.int64_data_[r] = std::get<int64_t>(v); stored_value = col.int64_data_[r]; break;
        case ColumnType::kDouble: col.double_data_[r] = std::get<double>(v); stored_value = col.double_data_[r]; break;
        case ColumnType::kString: {
          const uint32_t code = col.dictionary_.InternOrGet(std::get<std::string>(v));
          col.string_codes_[r] = code;
          stored_value = std::get<std::string>(v);
          break;
        }
      }
      if (!have_bounds) {
        col.min_ = stored_value;
        col.max_ = stored_value;
        have_bounds = true;
      } else {
        if (CompareValues(stored_value, col.min_) < 0) col.min_ = stored_value;
        if (CompareValues(stored_value, col.max_) > 0) col.max_ = stored_value;
      }
    }
    col.checksum_ = ComputeColumnChecksum(col);
  }
  return seg;
}

bool Segment::SaveToFile(const std::string& path, std::string* error) const {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    *error = "failed to open '" + path + "' for writing";
    return false;
  }
  out.write(kMagic, sizeof(kMagic));
  const uint32_t num_columns = static_cast<uint32_t>(columns_.size());
  WriteRaw(out, num_columns);
  WriteRaw(out, segment_id_);
  const uint32_t num_rows = static_cast<uint32_t>(num_rows_);
  WriteRaw(out, num_rows);

  for (const Column& col : columns_) {
    const uint8_t type_byte = static_cast<uint8_t>(col.type());
    WriteRaw(out, type_byte);
    const uint32_t null_count = static_cast<uint32_t>(num_rows_ - col.validity().CountValid());
    WriteRaw(out, null_count);
    WriteBytes(out, col.validity().data(), col.validity().byte_size());

    const bool has_min_max = !std::holds_alternative<std::monostate>(col.min());
    const uint8_t has_min_max_byte = has_min_max ? 1 : 0;
    WriteRaw(out, has_min_max_byte);
    if (has_min_max) {
      WriteTypedValue(out, col.type(), col.min());
      WriteTypedValue(out, col.type(), col.max());
    }

    WriteRaw(out, col.checksum());
    WriteBytes(out, DataPointer(col), num_rows_ * ElementWidth(col.type()));

    if (col.type() == ColumnType::kString) {
      const uint32_t dict_size = static_cast<uint32_t>(col.dictionary().size());
      WriteRaw(out, dict_size);
      for (const auto& s : col.dictionary().values()) {
        WriteString(out, s);
      }
    }
  }

  if (!out) {
    *error = "write failure while saving '" + path + "'";
    return false;
  }
  return true;
}

std::optional<Segment> Segment::LoadFromFile(const std::string& path, const Schema& schema,
                                              std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    *error = "failed to open '" + path + "' for reading";
    return std::nullopt;
  }

  char magic[8];
  if (!ReadBytes(in, magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) {
    *error = "'" + path + "': bad magic (not a CoreDB segment file, or file is truncated)";
    return std::nullopt;
  }

  uint32_t num_columns = 0;
  uint64_t segment_id = 0;
  uint32_t num_rows = 0;
  if (!ReadRaw(in, &num_columns) || !ReadRaw(in, &segment_id) || !ReadRaw(in, &num_rows)) {
    *error = "'" + path + "': truncated header";
    return std::nullopt;
  }
  if (num_columns != schema.size()) {
    *error = "'" + path + "': column count " + std::to_string(num_columns) +
             " does not match schema size " + std::to_string(schema.size());
    return std::nullopt;
  }

  Segment seg;
  seg.segment_id_ = segment_id;
  seg.schema_ = schema;
  seg.num_rows_ = num_rows;
  seg.columns_.resize(num_columns);

  for (uint32_t c = 0; c < num_columns; ++c) {
    Column& col = seg.columns_[c];
    uint8_t type_byte = 0;
    if (!ReadRaw(in, &type_byte)) {
      *error = "'" + path + "': truncated column header at column " + std::to_string(c);
      return std::nullopt;
    }
    col.type_ = static_cast<ColumnType>(type_byte);
    if (col.type_ != schema[c].type) {
      *error = "'" + path + "': column " + std::to_string(c) + " type mismatch (file has " +
               ToString(col.type_) + ", schema expects " + ToString(schema[c].type) + ")";
      return std::nullopt;
    }

    uint32_t null_count = 0;
    if (!ReadRaw(in, &null_count)) {
      *error = "'" + path + "': truncated null count at column " + std::to_string(c);
      return std::nullopt;
    }

    std::vector<uint8_t> bitmap_bytes((num_rows + 7) / 8);
    if (!bitmap_bytes.empty() && !ReadBytes(in, bitmap_bytes.data(), bitmap_bytes.size())) {
      *error = "'" + path + "': truncated validity bitmap at column " + std::to_string(c);
      return std::nullopt;
    }
    col.validity_ = util::Bitmap::FromBytes(num_rows, std::move(bitmap_bytes));
    if (num_rows - col.validity_.CountValid() != null_count) {
      *error = "'" + path + "': null count mismatch at column " + std::to_string(c) +
               " (bitmap disagrees with stored count -> corruption)";
      return std::nullopt;
    }

    uint8_t has_min_max_byte = 0;
    if (!ReadRaw(in, &has_min_max_byte)) {
      *error = "'" + path + "': truncated min/max flag at column " + std::to_string(c);
      return std::nullopt;
    }
    if (has_min_max_byte) {
      if (!ReadTypedValue(in, col.type_, &col.min_) || !ReadTypedValue(in, col.type_, &col.max_)) {
        *error = "'" + path + "': truncated min/max at column " + std::to_string(c);
        return std::nullopt;
      }
    }

    uint32_t stored_checksum = 0;
    if (!ReadRaw(in, &stored_checksum)) {
      *error = "'" + path + "': truncated checksum at column " + std::to_string(c);
      return std::nullopt;
    }

    const size_t data_bytes = static_cast<size_t>(num_rows) * ElementWidth(col.type_);
    switch (col.type_) {
      case ColumnType::kInt32: col.int32_data_.resize(num_rows); break;
      case ColumnType::kInt64: col.int64_data_.resize(num_rows); break;
      case ColumnType::kDouble: col.double_data_.resize(num_rows); break;
      case ColumnType::kString: col.string_codes_.resize(num_rows); break;
    }
    if (data_bytes > 0 && !ReadBytes(in, const_cast<void*>(DataPointer(col)), data_bytes)) {
      *error = "'" + path + "': truncated data block at column " + std::to_string(c);
      return std::nullopt;
    }

    if (col.type_ == ColumnType::kString) {
      uint32_t dict_size = 0;
      if (!ReadRaw(in, &dict_size)) {
        *error = "'" + path + "': truncated dictionary size at column " + std::to_string(c);
        return std::nullopt;
      }
      std::vector<std::string> values(dict_size);
      for (uint32_t i = 0; i < dict_size; ++i) {
        if (!ReadString(in, &values[i])) {
          *error = "'" + path + "': truncated dictionary entry at column " + std::to_string(c);
          return std::nullopt;
        }
      }
      col.dictionary_ = Dictionary::FromValues(std::move(values));
    }

    const uint32_t recomputed = ComputeColumnChecksum(col);
    if (recomputed != stored_checksum) {
      *error = "'" + path + "': checksum mismatch at column " + std::to_string(c) +
               " (expected " + std::to_string(stored_checksum) + ", got " +
               std::to_string(recomputed) + ") -> data corruption detected";
      return std::nullopt;
    }
    col.checksum_ = stored_checksum;
  }

  return seg;
}

}  // namespace coredb::storage
