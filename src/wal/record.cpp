#include "coredb/wal/record.h"

#include <cstring>

namespace coredb::wal {

using storage::Row;
using storage::Value;

const char* ToString(WalRecordType type) {
  switch (type) {
    case WalRecordType::kBegin: return "BEGIN";
    case WalRecordType::kInsert: return "INSERT";
    case WalRecordType::kUpdate: return "UPDATE";
    case WalRecordType::kDelete: return "DELETE";
    case WalRecordType::kCommit: return "COMMIT";
    case WalRecordType::kAbort: return "ABORT";
  }
  return "UNKNOWN";
}

namespace {

enum class ValueTag : uint8_t { kNull = 0, kInt32 = 1, kInt64 = 2, kDouble = 3, kString = 4 };

template <typename T>
void AppendRaw(std::vector<uint8_t>& buf, const T& v) {
  const auto* p = reinterpret_cast<const uint8_t*>(&v);
  buf.insert(buf.end(), p, p + sizeof(T));
}

template <typename T>
bool ReadRaw(const std::vector<uint8_t>& buf, size_t& pos, T* out) {
  if (pos + sizeof(T) > buf.size()) return false;
  std::memcpy(out, buf.data() + pos, sizeof(T));
  pos += sizeof(T);
  return true;
}

void AppendValue(std::vector<uint8_t>& buf, const Value& v) {
  if (std::holds_alternative<std::monostate>(v)) {
    buf.push_back(static_cast<uint8_t>(ValueTag::kNull));
  } else if (const auto* i = std::get_if<int32_t>(&v)) {
    buf.push_back(static_cast<uint8_t>(ValueTag::kInt32));
    AppendRaw(buf, *i);
  } else if (const auto* i = std::get_if<int64_t>(&v)) {
    buf.push_back(static_cast<uint8_t>(ValueTag::kInt64));
    AppendRaw(buf, *i);
  } else if (const auto* d = std::get_if<double>(&v)) {
    buf.push_back(static_cast<uint8_t>(ValueTag::kDouble));
    AppendRaw(buf, *d);
  } else {
    const auto& s = std::get<std::string>(v);
    buf.push_back(static_cast<uint8_t>(ValueTag::kString));
    const uint32_t len = static_cast<uint32_t>(s.size());
    AppendRaw(buf, len);
    buf.insert(buf.end(), s.begin(), s.end());
  }
}

bool ReadValue(const std::vector<uint8_t>& buf, size_t& pos, Value* out) {
  if (pos >= buf.size()) return false;
  const auto tag = static_cast<ValueTag>(buf[pos++]);
  switch (tag) {
    case ValueTag::kNull: *out = std::monostate{}; return true;
    case ValueTag::kInt32: {
      int32_t v;
      if (!ReadRaw(buf, pos, &v)) return false;
      *out = v;
      return true;
    }
    case ValueTag::kInt64: {
      int64_t v;
      if (!ReadRaw(buf, pos, &v)) return false;
      *out = v;
      return true;
    }
    case ValueTag::kDouble: {
      double v;
      if (!ReadRaw(buf, pos, &v)) return false;
      *out = v;
      return true;
    }
    case ValueTag::kString: {
      uint32_t len;
      if (!ReadRaw(buf, pos, &len)) return false;
      if (pos + len > buf.size()) return false;
      *out = std::string(buf.begin() + static_cast<long>(pos), buf.begin() + static_cast<long>(pos + len));
      pos += len;
      return true;
    }
  }
  return false;
}

}  // namespace

std::vector<uint8_t> EncodeBody(const WalRecord& record) {
  std::vector<uint8_t> buf;
  buf.reserve(32);
  AppendRaw(buf, record.lsn);
  AppendRaw(buf, record.txn_id);
  buf.push_back(static_cast<uint8_t>(record.type));

  if (record.type == WalRecordType::kInsert || record.type == WalRecordType::kUpdate) {
    AppendRaw(buf, record.row_id);
    const uint32_t num_values = static_cast<uint32_t>(record.row.size());
    AppendRaw(buf, num_values);
    for (const auto& v : record.row) AppendValue(buf, v);
  } else if (record.type == WalRecordType::kDelete) {
    AppendRaw(buf, record.row_id);
  }
  return buf;
}

bool DecodeBody(const std::vector<uint8_t>& body, WalRecord* out) {
  size_t pos = 0;
  uint8_t type_byte;
  if (!ReadRaw(body, pos, &out->lsn)) return false;
  if (!ReadRaw(body, pos, &out->txn_id)) return false;
  if (!ReadRaw(body, pos, &type_byte)) return false;
  out->type = static_cast<WalRecordType>(type_byte);

  if (out->type == WalRecordType::kInsert || out->type == WalRecordType::kUpdate) {
    if (!ReadRaw(body, pos, &out->row_id)) return false;
    uint32_t num_values = 0;
    if (!ReadRaw(body, pos, &num_values)) return false;
    out->row.clear();
    out->row.reserve(num_values);
    for (uint32_t i = 0; i < num_values; ++i) {
      Value v;
      if (!ReadValue(body, pos, &v)) return false;
      out->row.push_back(std::move(v));
    }
  } else if (out->type == WalRecordType::kDelete) {
    if (!ReadRaw(body, pos, &out->row_id)) return false;
  }
  return true;
}

}  // namespace coredb::wal
