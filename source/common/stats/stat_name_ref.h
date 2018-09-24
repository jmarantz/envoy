#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "envoy/common/pure.h"
#include "envoy/stats/stats.h"

#include "common/common/hash.h"
#include "common/stats/symbol_table_impl.h"

#include "absl/strings/string_view.h"

namespace Envoy {
namespace Stats {

// Helper classes to manage hash-tables that do mixed name lookups with either
// elaborated strings or symbolized StatNames. Note that hash-tables have to
// be constructed with explictly constructed Hash and Compare objects as they
// need the symbol-table for context.
class StatNameRef {
public:
  virtual ~StatNameRef() {}
  virtual uint64_t hash(const SymbolTable& symbolTable) const PURE;
  virtual bool compare(const SymbolTable& symbolTable, const StatNameRef& that) const PURE;
  virtual bool compareString(const SymbolTable& symbolTable, absl::string_view that) const PURE;
  virtual bool compareStatName(const SymbolTable& symbolTable, const StatName& that) const PURE;
};

using StatNameRefPtr = std::unique_ptr<StatNameRef>;

class StringViewStatNameRef : public StatNameRef {
 public:
  explicit StringViewStatNameRef(const absl::string_view name) : name_(name) {}
  uint64_t hash(const SymbolTable&) const override { return HashUtil::xxHash64(name_); }
  bool compare(const SymbolTable& symbol_table, const StatNameRef& that) const override {
    return that.compareString(symbol_table, name_);
  }
  bool compareString(const SymbolTable&, absl::string_view str) const override {
    return name_ == str;
  }
  bool compareStatName(const SymbolTable& symbol_table, const StatName& stat_name) const override {
    return symbol_table.compareString(stat_name, name_);
  }

 private:
  absl::string_view name_;
};

class SymbolStatNameRef : public StatNameRef {
 public:
  SymbolStatNameRef(const StatName& name) : name_(name) {}
  uint64_t hash(const SymbolTable& symbol_table) const override { return symbol_table.hash(name_); }
  bool compare(const SymbolTable& symbol_table, const StatNameRef& that) const override {
    return that.compareStatName(symbol_table, name_);
  }
  bool compareString(const SymbolTable& symbol_table, absl::string_view str) const override {
    return symbol_table.compareString(name_, str);
  }
  bool compareStatName(const SymbolTable&, const StatName& stat_name) const override {
    return stat_name == name_;
  }

 private:
  StatName name_;
};

struct StatNameRefPtrHash {
  StatNameRefPtrHash(const SymbolTable& symbol_table) : symbol_table_(symbol_table) {}
  size_t operator()(const StatNamePtr& a) const { return a->hash(symbol_table_); }

  const SymbolTable& symbol_table_;
};

struct StatNameRefPtrCompare {
  StatNameRefPtrCompare(const SymbolTable& symbol_table) : symbol_table_(symbol_table) {}
  bool operator()(const StatNamePtr& a, const StatNamePtr& b) const {
    return a->compare(symbol_table_, *b);
  }

  const SymbolTable& symbol_table_;
};

struct StatNameRefStarHash {
  StatNameRefStarHash(const SymbolTable& symbol_table) : symbol_table_(symbol_table) {}
  size_t operator()(const StatNameRef* a) const { return a->hash(symbol_table_); }

  const SymbolTable& symbol_table_;
};

struct StatNameRefStarCompare {
  StatNameRefStarCompare(const SymbolTable& symbol_table) : symbol_table_(symbol_table) {}
  bool operator()(const StatNameRef* a, const StatNameRef* b) const {
    return a->compare(symbol_table_, *b);
  }

  const SymbolTable& symbol_table_;
};

} // namespace Stats
} // namespace Envoy
