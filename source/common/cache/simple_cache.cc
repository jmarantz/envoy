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

class SimpleInsertContext : public InsertContext {
public:
  SimpleInsertContext(const Key& key, BackendSharedPtr cache) : key_(key), cache_(cache) {}

  virtual ~SimpleInsertContext() {
    // If a live cache has an uncommitted write, remove the 'in-progress' entry.
    if (!committed_) {
      BackendSharedPtr cache = cache_;
      if (cache.get() != nullptr) {
        cache = cache->self();
        if (cache.get() != nullptr) {
          cache->remove(key_, nullptr);
        }
      }
    }
  }

  void write(Value value, NotifyFn ready_for_next_chunk) override {
    ASSERT(!committed_);
    BackendSharedPtr cache = cache_;
    if (cache.get() != nullptr) {
      cache = cache->self();
      if (cache.get() != nullptr) {
        if (ready_for_next_chunk == nullptr) { // Final chunk
          if (value_.get() == nullptr) {
            value_ = value;
          } else {
            absl::StrAppend(&value_->value_, value->value_);
            value_->timestamp_ = value->timestamp_;
          }
          committed_ = true;
          static_cast<SimpleCache*>(cache.get())->insertHelper(key_, value_);
        } else {
          if (value_.get() == nullptr) {
            value_ = std::make_shared<ValueStruct>();
          }
          absl::StrAppend(&value_->value_, value->value_);
          ready_for_next_chunk(true);
        }
      }
    }
  }

private:
  Value value_;
  Key key_;
  BackendSharedPtr cache_;
  bool committed_ = false;
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

InsertContextPtr SimpleCache::insert(const Key& key) {
  Value value;
  {
    Thread::LockGuard lock(mutex_);
    map_[key] = value; // Empty value indicates InsertInProgress.
  }
  return std::make_unique<SimpleInsertContext>(key, self());
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
void SimpleCache::insertHelper(const Key& key, Value value) {
  // Only lock the cache and write the map when we have the complete value.
  Thread::LockGuard lock(mutex_);
  map_[key] = value;
}

CacheInfo SimpleCache::cacheInfo() const {
  CacheInfo cache_info;
  cache_info.chunk_size_bytes_ = 100 * 000;
  cache_info.max_size_bytes_ = 10 * 1000 * 1000;
  return cache_info;
}

} // namespace Cache
} // namespace Envoy
