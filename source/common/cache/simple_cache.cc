#include "common/cache/simple_cache.h"

#include "common/cache/cache.h"
#include "common/common/lock_guard.h"

#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"

namespace Envoy {
namespace Cache {

void SimpleCache::lookup(const Key& key, DataReceiverFn receiver) {
  BackendSharedPtr cache = self();
  Value value;
  auto status = DataStatus::kNotFound;
  if (cache.get() != nullptr) {
    Thread::LockGuard lock(mutex_);
    auto iter = map_.find(key);
    if (iter != map_.end()) {
      status = DataStatus::kLastChunk;
      value = iter->second;
    }
    // Release the lock before calling the receiver, retaining
    // access the ref-counted value, even if it’s evicted by another
    // thread immediately after releasing the lock.
  }
  receiver(status, value);
}

DataReceiverFn SimpleCache::insert(const Key& key) {
  remove(key, nullptr);  // Avoid reading stale values during the insertion.
  Value value;
  return [this, key, value](DataStatus status, const Value& chunk) -> ReceiverStatus {
    return InsertHelper(status, key, value, chunk);
  };
}

void SimpleCache::remove(const Key& key, NotifyFn confirm_fn) {
  {
    Thread::LockGuard lock(mutex_);
    map_.erase(key);
  }
  if (confirm_fn != nullptr) {
    confirm_fn(true);
  }
}

// Called by cache user for each chunk to insert into a key.
  ReceiverStatus SimpleCache::InsertHelper(
      DataStatus status, Key key, Value value, const Value& chunk) {
  switch (status) {
  case DataStatus::kNotFound:
  case DataStatus::kError:
    return ReceiverStatus::kAbort;
  case DataStatus::kChunksImminent:
  case DataStatus::kChunksPending:
    absl::StrAppend(&value->value_, chunk->value_);
    break;
  case DataStatus::kLastChunk:
    if (value.get() == nullptr) {
      // If the insertion occurred in one chunk, we don't need to copy any bytes.
      value = chunk;
    } else {
      // If the insertion was streamed in, we must accumulate the bytes.
      absl::StrAppend(&value->value_, chunk->value_);
      value->timestamp_ = chunk->timestamp_;
    }
    {
      // Only lock the cache and write the map when we have the complete value.
      Thread::LockGuard lock(mutex_);
      map_[key] = value;
    }
  }
  return ReceiverStatus::kOk;
}

CacheInfo SimpleCache::cacheInfo() const {
  CacheInfo cache_info;
  cache_info.chunk_size_bytes_ = 100 * 000;
  cache_info.max_size_bytes_ = 10 * 1000 * 1000;
  return cache_info;
}


}  // namespace Cache
}  // namespace Envoy
