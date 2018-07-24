#include "common/cache/simple_cache.h"

#include "common/cache/ecache.h"
#include "common/common/lock_guard.h"

#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"

namespace Envoy {
namespace ecache {

void SimpleCache::Lookup(const Key& key, DataReceiverFn receiver) {
  auto status = DataStatus::kNotFound;
  Value value;
  {
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

DataReceiverFn SimpleCache::Insert(const Key& key) {
  Delete(key, nullptr);  // Avoid reading stale values during the insertion.
  Value value;
  return [this, key, value](DataStatus status, const Value& chunk) -> ReceiverStatus {
    return InsertHelper(status, key, value, chunk);
  };
}

void SimpleCache::Delete(const Key& key, NotifyFn confirm_fn) {
  {
    Thread::LockGuard lock(mutex_);
    map_.erase(key);
  }
  if (confirm_fn == nullptr) {
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
    absl::StrAppend(&value->value, chunk->value);
    break;
  case DataStatus::kLastChunk:
    if (value->value.empty()) {
      // If the insertion occurred in one chunk, we don't need to copy any bytes.
      value = chunk;
    } else {
      // If the insertion was streamed in, we must accumulate the bytes.
      absl::StrAppend(&value->value, chunk->value);
      value->timestamp = chunk->timestamp;
    }
    {
      // Only lock the cache and write the map when we have the complete value.
      Thread::LockGuard lock(mutex_);
      map_[key] = value;
    }
  }
  return ReceiverStatus::kOk;
}

}  // namespace ecache
}  // namespace Envoy
