#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>

#include "common/stats/stat_name_ref.h"

#include "common/common/hash.h"
#include "common/common/lock_guard.h"
#include "common/common/thread.h"
#include "common/common/thread_annotations.h"
#include "common/stats/stat_data_allocator_impl.h"
#include "common/stats/symbol_table_impl.h"

#define FLAT_HASH 1
#if FLAT_HASH

#include "absl/container/flat_hash_set.h"

#else

#include <unordered_set>

#endif

namespace Envoy {
namespace Stats {

class HeapStatDataAllocator;

/**
 * This structure is an alternate backing store for both CounterImpl and GaugeImpl. It is designed
 * so that it can be allocated efficiently from the heap on demand.
 */
struct HeapStatData {
  explicit HeapStatData(const SymbolVec& symbol_vec) {
    StatName stat_name;
    stat_name.init(symbol_vec, name_);
  }

  /**
   * @returns std::string the name as a std::string with no truncation.
   */
  std::string name(const SymbolTable& symbol_table) const;
  StatNameRef nameRef() const { return StatNameRef(StatName(name_)); }

  bool operator==(const HeapStatData& rhs) const { return StatName(name_) == StatName(rhs.name_); }

  std::atomic<uint64_t> value_{0};
  std::atomic<uint64_t> pending_increment_{0};
  std::atomic<uint16_t> flags_{0};
  std::atomic<uint16_t> ref_count_{1};
  uint8_t name_[];
};

/**
 * Implementation of StatDataAllocator using a pure heap-based strategy, so that
 * Envoy implementations that do not require hot-restart can use less memory.
 */
class HeapStatDataAllocator : public StatDataAllocatorImpl<HeapStatData> {
public:
  explicit HeapStatDataAllocator(SymbolTable& symbol_table);
  ~HeapStatDataAllocator();

  // StatDataAllocatorImpl
  HeapStatData* alloc(absl::string_view name) override;
  void free(HeapStatData& data) override;

  // StatDataAllocator
  bool requiresBoundedStatNameSize() const override { return false; }

  // SymbolTable
  //StatName encode(absl::string_view sv) { return table_.encode(sv); }
  const SymbolTable& symbolTable() const override { return table_; }
  SymbolTable& symbolTable() override { return table_; }

  int64_t bytesSaved() const { return bytes_saved_; }

private:
  friend HeapStatData;

  struct HeapStatHash {
    size_t operator()(const HeapStatData* a) const { return StatName(a->name_).hash(); }
  };
  struct HeapStatCompare {
    bool operator()(const HeapStatData* a, const HeapStatData* b) const { return *a == *b; }
  };

#if FLAT_HASH
  typedef absl::flat_hash_set<HeapStatData*, HeapStatHash, HeapStatCompare> StatSet;
#else
  typedef std::unordered_set<HeapStatData*, HeapStatHash, HeapStatCompare> StatSet;
#endif

  // An unordered set of HeapStatData pointers which keys off the key()
  // field in each object. This necessitates a custom comparator and hasher, which key off of the
  // StatNamePtr's own StatNamePtrHash and StatNamePtrCompare operators.
  StatSet stats_ GUARDED_BY(mutex_);
  // A locally held symbol table which encodes stat names as StatNamePtrs and decodes StatNamePtrs
  // back into strings. This does not get guarded by mutex_, since it has its own internal mutex to
  // guarantee thread safety.
  SymbolTable& table_;
  // A mutex is needed here to protect both the stats_ object from both
  // alloc() and free() operations. Although alloc() operations are called under existing locking,
  // free() operations are made from the destructors of the individual stat objects, which are not
  // protected by locks.
  Thread::MutexBasicLockable mutex_;
  size_t bytes_saved_{0};
};

} // namespace Stats
} // namespace Envoy

#undef FLAT_HASH
