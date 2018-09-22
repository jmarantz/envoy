#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "envoy/common/pure.h"

#include "common/common/hash.h"

#include "absl/strings/string_view.h"

namespace Envoy {
namespace Stats {

class StatNameRef {
public:
  virtual ~StatNameRef() {}
  virtual uint64_t hash() const PURE;
  virtual bool operator==(const StatNameRef& that) const PURE;
};

class StringViewStatNameRef : public StatNameRef {
 public:
  StringViewStatNameRef(const absl::string_view name) : name_(name) {}
  uint64_t hash() const { return HashUtil::xxHash64(name_); }
  bool operator==(StatNameRef& that) const {
    StringViewStatNameRef* ref = dynamic_cast<StringViewStatNameRef*>(&that);
    if (ref == nullptr) {
      return false;
    }
    return ref->name_ == name_;
  }
 private:
  absl::string_view name_;
};

struct StatNameRefHash {
  size_t operator()(const StatNameRef& a) const { return a.hash(); }
};

/*
struct StatNameRefCompare {
  StatNameRefCompare(const SymbolTable& symbol_table) : symbol_table_(symbol_table_) {}
  bool operator()(const StatNameRef& a, const StatNameRef& b) const {
    if (a == b) {
      return true;

return a.hash(); }
  const SymbolTable& symbol_table_;
};*/

} // namespace Stats
} // namespace Envoy
