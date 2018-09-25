#include "common/stats/heap_stat_data.h"

#include "common/common/lock_guard.h"
#include "common/common/thread.h"

namespace Envoy {
namespace Stats {

//HeapStatData::HeapStatData(StatNamePtr name_ptr) : name_ptr_(std::move(name_ptr)) {}

HeapStatDataAllocator::HeapStatDataAllocator(SymbolTable& symbol_table)
    : table_(symbol_table) {}

HeapStatDataAllocator::~HeapStatDataAllocator() { ASSERT(stats_.empty()); }

HeapStatData* HeapStatDataAllocator::alloc(absl::string_view name) {
  // Any expected truncation of name is done at the callsite. No truncation is
  // required to use this allocator.
  SymbolVec symbol_vec = table_.encode(name);
  size_t encoded_bytes = StatName::size(symbol_vec);
  bytes_saved_ += name.size() + 1 - encoded_bytes;
  void* memory = malloc(sizeof(HeapStatData) + encoded_bytes);
  std::unique_ptr<HeapStatData> data(new (memory) HeapStatData(symbol_vec));
  Thread::ReleasableLockGuard lock(mutex_);
  auto ret = stats_.insert(data.get());
  lock.release();
  HeapStatData* existing_data = *ret.first;

  if (ret.second) {
    return data.release();
  }
  ++existing_data->ref_count_;
  return existing_data;
}

void HeapStatDataAllocator::free(HeapStatData& data) {
  ASSERT(data.ref_count_ > 0);
  if (--data.ref_count_ > 0) {
    return;
  }

  {
    Thread::LockGuard lock(mutex_);
    size_t key_removed = stats_.erase(&data);
    ASSERT(key_removed == 1);
  }
  StatName(data.name_).free(table_);

  delete &data;
}

std::string HeapStatData::name(const SymbolTable& symbol_table) const {
  return StatName(name_).toString(symbol_table);
}

template class StatDataAllocatorImpl<HeapStatData>;

} // namespace Stats
} // namespace Envoy
