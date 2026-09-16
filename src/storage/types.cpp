#include "coredb/storage/types.h"

#include <cmath>
#include <stdexcept>

namespace coredb::storage {

const char* ToString(ColumnType type) {
  switch (type) {
    case ColumnType::kInt32: return "int32";
    case ColumnType::kInt64: return "int64";
    case ColumnType::kDouble: return "double";
    case ColumnType::kString: return "string";
  }
  return "unknown";
}

const char* ToString(PredicateOp op) {
  switch (op) {
    case PredicateOp::kEq: return "=";
    case PredicateOp::kNe: return "!=";
    case PredicateOp::kLt: return "<";
    case PredicateOp::kLe: return "<=";
    case PredicateOp::kGt: return ">";
    case PredicateOp::kGe: return ">=";
  }
  return "?";
}

namespace {

bool IsNumeric(const Value& v) {
  return std::holds_alternative<int32_t>(v) || std::holds_alternative<int64_t>(v) ||
         std::holds_alternative<double>(v);
}

double AsDouble(const Value& v) {
  if (const auto* i = std::get_if<int32_t>(&v)) return static_cast<double>(*i);
  if (const auto* i = std::get_if<int64_t>(&v)) return static_cast<double>(*i);
  if (const auto* d = std::get_if<double>(&v)) return *d;
  throw std::logic_error("AsDouble called on non-numeric Value");
}

}  // namespace

int CompareValues(const Value& lhs, const Value& rhs) {
  if (std::holds_alternative<std::string>(lhs) && std::holds_alternative<std::string>(rhs)) {
    const auto& a = std::get<std::string>(lhs);
    const auto& b = std::get<std::string>(rhs);
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
  }
  if (IsNumeric(lhs) && IsNumeric(rhs)) {
    const double a = AsDouble(lhs);
    const double b = AsDouble(rhs);
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
  }
  throw std::logic_error("CompareValues: incompatible or NULL operands");
}

bool EvaluatePredicate(const Value& column_value, PredicateOp op, const Value& literal) {
  if (std::holds_alternative<std::monostate>(column_value)) {
    // SQL NULL: every comparison against NULL is unknown, which filters
    // treat as "not matched".
    return false;
  }
  const int cmp = CompareValues(column_value, literal);
  switch (op) {
    case PredicateOp::kEq: return cmp == 0;
    case PredicateOp::kNe: return cmp != 0;
    case PredicateOp::kLt: return cmp < 0;
    case PredicateOp::kLe: return cmp <= 0;
    case PredicateOp::kGt: return cmp > 0;
    case PredicateOp::kGe: return cmp >= 0;
  }
  return false;
}

}  // namespace coredb::storage
