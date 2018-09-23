#include "common/stats/symbol_table_impl.h"

#include <memory>
#include <unordered_map>
#include <vector>

#include "common/common/assert.h"

namespace Envoy {
namespace Stats {

// TODO(ambuc): There is a possible performance optimization here for avoiding the encoding of IPs /
// numbers if they appear in stat names. We don't want to waste time symbolizing an integer as an
// integer, if we can help it.
SymbolVec SymbolTable::encode(const absl::string_view name) {
  auto tokens = absl::StrSplit(name, '.');
  SymbolVec symbol_vec;
  Thread::LockGuard lock(lock_);
  for (absl::string_view token : tokens) {
    Symbol symbol = toSymbol(token);
    // high-order bit of each byte indicates there is more to come.
    do {
      if (symbol < (1 << 7)) {
        symbol_vec += static_cast<char>(symbol);
      } else {
        symbol_vec += static_cast<char>((symbol & 0x7f) | 0x80);
      }
      symbol >>= 7;
    } while (symbol != 0);
  }
  return symbol_vec;
}

bool SymbolTable::nextChar(uint8_t c, Symbol& symbol, int& shift) {
  uint32_t uc = static_cast<uint32_t>(c);
  symbol |= (uc & 0x7f) << shift;
  if ((uc & 0x80) == 0) {
    shift = 0;
    return true;
  }
  shift += 7;
  return false;
}

std::string SymbolTable::decode(const uint8_t* symbol_vec, size_t size) const {
  std::vector<absl::string_view> name;
  {
    Thread::LockGuard lock(lock_);
    Symbol symbol = 0;
    int shift = 0;
    for (size_t i = 0; i < size; ++i) {
      if (nextChar(symbol_vec[i], symbol, shift)) {
        name.push_back(fromSymbol(symbol));
        symbol = 0;
      }
    }
  }
  return absl::StrJoin(name, ".");
}

/*std::string SymbolTable::decode(const SymbolVec& symbol_vec) const {
  uint32_t symbol_value = 0;
  std::vector<uint32_t> symbols;
  std::vector<absl::string_view> name;
  {
    Thread::LockGuard lock(lock_);
    for (uint8_t c : symbol_vec) {
      uint32_t uc = static_cast<uint32_t>(c);
      if (uc & 0x80) {
        symbol_value |= (uc & 0x7f) << 7;
      } else {
        name.push_back(fromSymbol(symbol_value | uc));
        symbol_value = 0;
      }
    }
  }
  return absl::StrJoin(name, ".");
  }*/

bool SymbolTable::compareString(const StatName& stat_name, const absl::string_view str) const {
  // TOOD(jmarantz): rather than elaboarating the string, it will be straightforward to
  // adapt the encode() algorithm and return false on the first mismatching token split
  // out from str. In the meantime it's easy to just allocate a temp and compare.
  return str == stat_name.toString(*this);
}

uint64_t SymbolTable::hash(const StatName& stat_name) const {
  // TOOD(jmarantz): we could memoize the hash instead of computing it. It would be
  // nicer to hash iteratively but we are treating XX64 as a black box.
  return HashUtil::xxHash64(stat_name.toString(*this));
}

void SymbolTable::free(uint8_t* symbol_vec, size_t size) {
  Thread::LockGuard lock(lock_);
  Symbol symbol = 0;
  int shift = 0;
  for (size_t i = 0; i < size; ++i) {
    if (nextChar(symbol_vec[i], symbol, shift)) {
      auto decode_search = decode_map_.find(symbol);
      ASSERT(decode_search != decode_map_.end());

      auto encode_search = encode_map_.find(decode_search->second);
      ASSERT(encode_search != encode_map_.end());

      encode_search->second.ref_count_--;
      // If that was the last remaining client usage of the symbol, erase the the current
      // mappings and add the now-unused symbol to the reuse pool.
      if (encode_search->second.ref_count_ == 0) {
        decode_map_.erase(decode_search);
        encode_map_.erase(encode_search);
        pool_.push(symbol);
      }
      symbol = 0;
    }
  }
}

Symbol SymbolTable::toSymbol(absl::string_view sv) EXCLUSIVE_LOCKS_REQUIRED(lock_) {
  Symbol result;
  auto encode_find = encode_map_.find(sv);
  // If the string segment doesn't already exist,
  if (encode_find == encode_map_.end()) {
    // We create the actual string, place it in the decode_map_, and then insert a string_view
    // pointing to it in the encode_map_. This allows us to only store the string once.
    std::string str = std::string(sv);

    auto decode_insert = decode_map_.insert({next_symbol_, std::move(str)});
    ASSERT(decode_insert.second);

    auto encode_insert = encode_map_.insert(
        {decode_insert.first->second, {.symbol_ = next_symbol_, .ref_count_ = 1}});
    ASSERT(encode_insert.second);

    result = next_symbol_;
    newSymbol();
  } else {
    // If the insertion didn't take place, return the actual value at that location and up the
    // refcount at that location
    result = encode_find->second.symbol_;
    ++(encode_find->second.ref_count_);
  }
  return result;
}

absl::string_view SymbolTable::fromSymbol(const Symbol symbol) const
    EXCLUSIVE_LOCKS_REQUIRED(lock_) {
  auto search = decode_map_.find(symbol);
  ASSERT(search != decode_map_.end());
  return search->second;
}

void SymbolTable::newSymbol() EXCLUSIVE_LOCKS_REQUIRED(lock_) {
  if (pool_.empty()) {
    next_symbol_ = ++monotonic_counter_;
  } else {
    next_symbol_ = pool_.top();
    pool_.pop();
  }
  // This should catch integer overflow for the new symbol.
  ASSERT(monotonic_counter_ != 0);
}

} // namespace Stats
} // namespace Envoy
