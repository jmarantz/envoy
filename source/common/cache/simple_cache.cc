#include "common/cache/simple_cache.h"

#include "common/cache/cache.h"
#include "common/common/lock_guard.h"

#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"

namespace Envoy {
namespace Cache {

class SimpleLookupContext : public LookupContext {
public:
  SimpleLookupContext(const Key& key, BackendSharedPtr cache) : key_(key), cache_(cache) {}

  void read(DataReceiverFn receiver) override {
    BackendSharedPtr cache = cache_;
    if (cache.get() != nullptr) {
      cache = cache->self();
      if (cache.get() != nullptr) {
        static_cast<SimpleCache*>(cache.get())->lookupHelper(key_, receiver);
        return;
      }
    }
    receiver(DataStatus::NotFound, Value());
  }

private:
  Key key_;
  BackendSharedPtr cache_;
};

LookupContextPtr SimpleCache::lookup(const Key& key) {
  return std::make_unique<SimpleLookupContext>(key, self());
}

void SimpleCache::lookupHelper(const Key& key, DataReceiverFn receiver) {
  Value value;
  auto status = DataStatus::NotFound;
  {
    Thread::LockGuard lock(mutex_);
    auto iter = map_.find(key);
    if (iter != map_.end()) {
      status = DataStatus::LastChunk;
      value = iter->second;
      if (value.get() == nullptr) {
        // The map entry was present, but the Value pointer contained null.
        // This indicates an insert is in progress.
        status = DataStatus::InsertInProgress;
      }
    }
  }
  // Release the lock before calling the receiver, retaining
  // access the ref-counted value, even if it’s evicted by another
  // thread immediately after releasing the lock.
  receiver(status, value);
}

DataReceiverFn SimpleCache::insert(const Key& key) {
  Value value;
  {
    Thread::LockGuard lock(mutex_);
    map_[key] = value; // Empty value indicates InsertInProgress.
  }
  remove(key, nullptr); // Avoid reading stale values during the insertion.
  return [this, key, value](DataStatus status, const Value& chunk) -> ReceiverStatus {
    return insertHelper(status, key, value, chunk);
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
ReceiverStatus SimpleCache::insertHelper(DataStatus status, Key key, Value value,
                                         const Value& chunk) {
  switch (status) {
  case DataStatus::NotFound:
  case DataStatus::Error:
  case DataStatus::InsertInProgress:
    return ReceiverStatus::Abort;
  case DataStatus::ChunksImminent:
  case DataStatus::ChunksPending:
    absl::StrAppend(&value->value_, chunk->value_);
    break;
  case DataStatus::LastChunk:
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
  return ReceiverStatus::Ok;
}

CacheInfo SimpleCache::cacheInfo() const {
  CacheInfo cache_info;
  cache_info.chunk_size_bytes_ = 100 * 000;
  cache_info.max_size_bytes_ = 10 * 1000 * 1000;
  return cache_info;
}

} // namespace Cache
} // namespace Envoy
