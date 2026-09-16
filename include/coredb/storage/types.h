#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace coredb::storage {

enum class ColumnType : uint8_t {
  kInt32 = 0,
  kInt64 = 1,
  kDouble = 2,
  kString = 3,
};

const char* ToString(ColumnType type);

struct ColumnSchema {
  std::string name;
  ColumnType type;
};

using Schema = std::vector<ColumnSchema>;

// A single cell value. `std::monostate` represents SQL NULL. This is the
// row-oriented representation used at the table API boundary (inserts,
// updates, WAL payloads) and inside the delta layer; base segments store
// columns in typed contiguous arrays instead (see Segment).
using Value = std::variant<std::monostate, int32_t, int64_t, double, std::string>;

using Row = std::vector<Value>;

enum class PredicateOp : uint8_t { kEq, kNe, kLt, kLe, kGt, kGe };

const char* ToString(PredicateOp op);

// Three-way compare for two Values of (expected) compatible type.
//   < 0  if lhs < rhs
//   = 0  if lhs == rhs
//   > 0  if lhs > rhs
// Numeric values compare numerically even across Int32/Int64/Double (by
// widening to double); strings compare lexicographically. Comparing a
// monostate (NULL) against anything is undefined per SQL null semantics, so
// callers must check for NULL themselves before calling this.
int CompareValues(const Value& lhs, const Value& rhs);

bool EvaluatePredicate(const Value& column_value, PredicateOp op, const Value& literal);

}  // namespace coredb::storage
