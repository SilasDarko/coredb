#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace coredb::storage {

// Maps distinct strings in a string column to small integer codes so the
// column body can be stored (and scanned/filtered) as a dense uint32_t
// array instead of variable-length string data. Codes are assigned in
// first-seen order starting at 0.
class Dictionary {
 public:
  // Returns the existing code for `value`, or inserts it with a fresh code.
  uint32_t InternOrGet(const std::string& value) {
    auto it = code_of_.find(value);
    if (it != code_of_.end()) return it->second;
    const uint32_t code = static_cast<uint32_t>(values_.size());
    values_.push_back(value);
    code_of_.emplace(value, code);
    return code;
  }

  // Read-only lookup used when a code is already known (e.g. decoding a
  // stored segment); returns false if `value` was never interned.
  bool TryGetCode(const std::string& value, uint32_t* code) const {
    auto it = code_of_.find(value);
    if (it == code_of_.end()) return false;
    *code = it->second;
    return true;
  }

  const std::string& ValueOf(uint32_t code) const { return values_.at(code); }
  size_t size() const { return values_.size(); }
  const std::vector<std::string>& values() const { return values_; }

  // Rebuilds a dictionary from a serialized value list (used when loading a
  // segment off disk).
  static Dictionary FromValues(std::vector<std::string> values) {
    Dictionary dict;
    dict.values_ = std::move(values);
    dict.code_of_.reserve(dict.values_.size());
    for (uint32_t code = 0; code < dict.values_.size(); ++code) {
      dict.code_of_.emplace(dict.values_[code], code);
    }
    return dict;
  }

 private:
  std::vector<std::string> values_;
  std::unordered_map<std::string, uint32_t> code_of_;
};

}  // namespace coredb::storage
