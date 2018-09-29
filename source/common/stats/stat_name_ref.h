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

// Helper class to manage hash-tables that do mixed name lookups with either
// elaborated strings or symbolized StatNames. Note that hash-tables have to
// be constructed with explictly constructed Hash and Compare objects as they
// need the symbol-table for context.
class StatNameRef {
public:
  static constexpr size_t StatNameMask = static_cast<uint64_t>(0x1);

  /*
  static constexpr size_t StatNameMarker = static_cast<size_t>(-1);
   StatNameRef(const StatName& name)
      : data_(reinterpret_cast<const char*>(name.rawData())), size_(StatNameMarker) {}
  //  StatNameRef(absl::string_view str) : data_(str.data()), size_(str.size()) {
  //    ASSERT(size_ != StatNameMarker);
  // }
  */

  StatNameRef(const StatName& name) : StatNameRef(reinterpret_cast<uint64_t>(name.rawData()), true) {}
  StatNameRef(const std::string& str) : StatNameRef(reinterpret_cast<uint64_t>(str.c_str()), false) {}
  StatNameRef(const char* str) : StatNameRef(reinterpret_cast<uint64_t>(str), false) {}
  StatNameRef(uint64_t data, bool is_stat_name) : data_(data | (is_stat_name ? StatNameMask : 0)) {
    ASSERT((data & StatNameMask) == 0);
  }

  bool isStatName() const { return (data_ & StatNameMask) != 0; }
  StatName statName() const {
    return StatName(reinterpret_cast<const uint8_t*>(data_ & ~StatNameMask));
  }
  absl::string_view stringView() const {
    return absl::string_view(reinterpret_cast<const char*>(data_));
  }

  /*
  bool isStatName() const { return size_ == StatNameMarker; }
  StatName statName() const { return StatName(reinterpret_cast<const uint8_t*>(data_)); }
  absl::string_view stringView() const { return absl::string_view(data_, size_); }
  */

  uint64_t hash(const SymbolTable& symbol_table) const {
    return isStatName() ? symbol_table.hash(statName()) : HashUtil::xxHash64(stringView());
  }

  bool compare(const SymbolTable& symbol_table, const StatNameRef& that) const {
    if (isStatName()) {
      if (that.isStatName()) {
        return statName() == that.statName();
      } else {
        return symbol_table.compareString(statName(), that.stringView());
      }
    } else if (that.isStatName()) {
      return symbol_table.compareString(that.statName(), stringView());
    }
    return stringView() == that.stringView();
  }

 private:
  const uint64_t data_;
  //const char* data_;
  //size_t size_;
};

using StatNameRefPtr = std::unique_ptr<StatNameRef>;

/*class StringViewStatNameRef : public StatNameRef {
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
  };*/

/*struct StatNameRefPtrHash {
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
  };*/

struct StatNameRefHash {
  StatNameRefHash(const SymbolTable& symbol_table) : symbol_table_(symbol_table) {}
  size_t operator()(const StatNameRef& a) const { return a.hash(symbol_table_); }

  const SymbolTable& symbol_table_;
};

struct StatNameRefCompare {
  StatNameRefCompare(const SymbolTable& symbol_table) : symbol_table_(symbol_table) {}
  bool operator()(const StatNameRef& a, const StatNameRef& b) const {
    return a.compare(symbol_table_, b);
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
