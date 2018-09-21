#include "common/stats/heap_stat_data.h"

#include "common/common/lock_guard.h"
#include "common/common/thread.h"

namespace Envoy {
namespace Stats {

HeapStatData::HeapStatData(StatNamePtr name_ptr) : name_ptr_(std::move(name_ptr)) {}

HeapStatDataAllocator::HeapStatDataAllocator() {}

HeapStatDataAllocator::~HeapStatDataAllocator() { ASSERT(stats_.empty()); }

HeapStatData* HeapStatDataAllocator::alloc(absl::string_view name) {
  // Any expected truncation of name is done at the callsite. No truncation is
  // required to use this allocator.
  auto data = std::make_unique<HeapStatData>(table_.encode(name));
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

  delete &data;
}

template class StatDataAllocatorImpl<HeapStatData>;

} // namespace Stats
} // namespace Envoy
