#pragma once

#include <algorithm>
#include <cstring>
#include <memory>
#include <stack>
#include <string>
#include <unordered_map>
#include <vector>

#include "envoy/common/exception.h"

#include "common/common/assert.h"
#include "common/common/hash.h"
#include "common/common/lock_guard.h"
#include "common/common/non_copyable.h"
#include "common/common/thread.h"
#include "common/common/utility.h"

#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"

namespace Envoy {
namespace Stats {

using Symbol = uint32_t;
using SymbolVec = std::string;
class StatName; // forward declaration
//using StatNamePtr = std::unique_ptr<StatName>;
//using StatNamePtr = StatName;

/**
 * Underlying SymbolTable implementation which manages per-symbol reference counting.
 *
 * The underlying Symbol / SymbolVec data structures are private to the impl. One side
 * effect of the non-monotonically-increasing symbol counter is that if a string is encoded, the
 * resulting stat is destroyed, and then that same string is re-encoded, it may or may not encode to
 * the same underlying symbol.
 */
class SymbolTable {
public:
  SymbolTable() {
    // Have to be explicitly declared, if we want to use the GUARDED_BY macro
    next_symbol_ = 0;
    monotonic_counter_ = 0;
  }

  SymbolVec encode(absl::string_view name);

  // For testing purposes only.
  size_t size() const {
    Thread::LockGuard lock(lock_);
    ASSERT(encode_map_.size() == decode_map_.size());
    return encode_map_.size();
  }

  uint64_t hash(const StatName& stat_name) const;
  bool compareString(const StatName& stat_name, const absl::string_view str) const;

private:
  friend class StatName;
  friend class StatNameTest;

  struct SharedSymbol {
    Symbol symbol_;
    uint32_t ref_count_;
  };

  // This must be called during both encode() and free().
  mutable Thread::MutexBasicLockable lock_;

  /**
   * Decodes a vector of symbols back into its period-delimited stat name.
   * If decoding fails on any part of the symbol_vec, we release_assert and crash hard, since this
   * should never happen, and we don't want to continue running with a corrupt stats set.
   *
   * @param symbol_vec the vector of symbols to decode.
   * @return std::string the retrieved stat name.
   */
  std::string decode(const uint8_t* symbol_vec, size_t size) const;

  /**
   * Since SymbolTable does manual reference counting, a client of SymbolTable (such as
   * StatName) must manually call free(symbol_vec) when it is freeing the stat it represents. This
   * way, the symbol table will grow and shrink dynamically, instead of being write-only.
   *
   * @param symbol_vec the vector of symbols to be freed.
   */
  void free(uint8_t* symbol_vec, size_t size);

  /**
   * Convenience function for encode(), symbolizing one string segment at a time.
   *
   * @param sv the individual string to be encoded as a symbol.
   * @return Symbol the encoded string.
   */
  Symbol toSymbol(absl::string_view sv);

  /**
   * Convenience function for decode(), decoding one symbol at a time.
   *
   * @param symbol the individual symbol to be decoded.
   * @return absl::string_view the decoded string.
   */
  absl::string_view fromSymbol(Symbol symbol) const;

  // Stages a new symbol for use. To be called after a successful insertion.
  void newSymbol();

  Symbol monotonicCounter() {
    Thread::LockGuard lock(lock_);
    return monotonic_counter_;
  }

  static bool nextChar(uint8_t c, Symbol& symbol, int& shift);

  // Stores the symbol to be used at next insertion. This should exist ahead of insertion time so
  // that if insertion succeeds, the value written is the correct one.
  Symbol next_symbol_ GUARDED_BY(lock_);

  // If the free pool is exhausted, we monotonically increase this counter.
  Symbol monotonic_counter_ GUARDED_BY(lock_);

  // Bimap implementation.
  // The encode map stores both the symbol and the ref count of that symbol.
  // Using absl::string_view lets us only store the complete string once, in the decode map.
  std::unordered_map<absl::string_view, SharedSymbol, StringViewHash> encode_map_ GUARDED_BY(lock_);
  std::unordered_map<Symbol, std::string> decode_map_ GUARDED_BY(lock_);

  // Free pool of symbols for re-use.
  // TODO(ambuc): There might be an optimization here relating to storing ranges of freed symbols
  // using an Envoy::IntervalSet.
  std::stack<Symbol> pool_ GUARDED_BY(lock_);
};

/**
 * Implements RAII for Symbols, since the StatName destructor does the work of freeing its component
 * symbols.
 */
class StatName : public NonCopyable {
public:
  explicit StatName(const SymbolVec& symbol_vec) {
    size_t size = symbol_vec.size();
    ASSERT(size < 65536);
    symbol_vec_ = new uint8_t[symbol_vec.size() + 2];
    symbol_vec_[0] = size & 0xff;
    symbol_vec_[1] = size >> 8;
    memcpy(data(), symbol_vec.data(), size * sizeof(uint8_t));
  }

  ~StatName() {
    delete [] symbol_vec_;
  }

  //~StatName() { ASSERT(symbol_vec_.empty()); }  // { symbolb_table_.free(symbol_vec_); }

  size_t size() const {
    return symbol_vec_[0] | (static_cast<size_t>(symbol_vec_[1]) << 8);
  }

  uint8_t* data() { return symbol_vec_ + 2; }
  const uint8_t* data() const { return symbol_vec_ + 2; }

  void free(SymbolTable& symbol_table) { symbol_table.free(data(), size()); }
  std::string toString(const SymbolTable& table) const { return table.decode(data(), size()); }

  // Returns a hash of the underlying symbol vector, since StatNames are uniquely defined by their
  // symbol vectors.
  uint64_t hash() const {
    const char* cdata = reinterpret_cast<const char*>(data());
    return HashUtil::xxHash64(absl::string_view(cdata, size()));
  }

  // Compares on the underlying symbol vectors.
  // NB: operator==(std::vector) checks size first, then compares equality for each element.
  bool operator==(const StatName& rhs) const {
    const size_t sz = size();
    return sz == rhs.size() && memcmp(data(), rhs.data(), sz * sizeof(uint8_t)) == 0;
  }
  bool operator!=(const StatName& rhs) const { return !(*this == rhs); }

private:
  friend SymbolTable;

  friend class StatNameTest;
  uint8_t* symbol_vec_;
};

struct StatNamePtrHash {
  size_t operator()(const StatName* a) const { return a->hash(); }
};

struct StatNamePtrCompare {
  bool operator()(const StatName* a, const StatName* b) const {
    return *a == *b;
  }
};

/*struct StatNameRefHash {
  size_t operator()(const StatName& a) const { return a.hash(); }
};

struct StatNameRefCompare {
  bool operator()(const StatName& a, const StatName& b) const {
    // This extracts the underlying statnames.
    return a == b;
  }
  };*/

struct StatNameUniquePtrHash {
  size_t operator()(const StatName& a) const { return a.hash(); }
};

struct StatNameUniquePtrCompare {
  bool operator()(const StatName& a, const StatName& b) const {
    // This extracts the underlying statnames.
    return a == b;
  }
};

} // namespace Stats
} // namespace Envoy
