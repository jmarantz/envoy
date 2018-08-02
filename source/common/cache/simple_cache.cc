#include "common/cache/simple_cache.h"

#include "common/cache/cache.h"
#include "common/common/lock_guard.h"

#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"

namespace Envoy {
namespace Cache {

using SimpleCacheSharedPtr = std::shared_ptr<SimpleCache>;

class SimpleLookupContext : public LookupContext {
public:
  SimpleLookupContext(const Descriptor& descriptor, SimpleCacheSharedPtr cache)
      : key_(std::string(descriptor.key())), split_(descriptor.hasAttribute("split")),
        cache_(cache) {}

  void read(DataReceiverFn receiver) override {
    SimpleCacheSharedPtr cache = cache_;
    if (cache.get() != nullptr && cache->isHealthy()) {
      if (!split_) {
        cache->lookupHelper(key_, receiver);
        return;
      } else if (residual_value_.get() == nullptr) {
        Value value;
        cache->lookupHelper(key_, [&value, this](DataStatus s, const Value& v) {
                                    value = v;
                                    residual_status_ = s;
                                    return ReceiverStatus::Ok;
                                  });
        Value v1 = std::make_shared<ValueStruct>();
        size_t size1 = value->value_.size() / 2;
        v1->value_ = value->value_.substr(0, size1); // timestamp not touched.
        residual_value_ = std::make_shared<ValueStruct>();
        residual_value_->value_ = value->value_.substr(size1);
        residual_value_->timestamp_ = value->timestamp_;
        receiver(DataStatus::ChunksImminent, v1);
      } else {
        receiver(residual_status_, residual_value_);
      }
      return;
    }
    receiver(DataStatus::NotFound, Value());
  }

private:
  std::string key_;
  bool split_;
  SimpleCacheSharedPtr cache_;
  Value residual_value_;
  DataStatus residual_status_;
};

class SimpleInsertContext : public InsertContext {
public:
  SimpleInsertContext(const Descriptor& descriptor, SimpleCacheSharedPtr cache)
      : key_(std::string(descriptor.key())), cache_(cache) {}

  virtual ~SimpleInsertContext() {
    // If a live cache has an uncommitted write, remove the 'in-progress' entry.
    if (!committed_) {
      SimpleCacheSharedPtr cache = cache_;
      if (cache.get() != nullptr && cache->isHealthy()) {
        cache->removeHelper(key_);
      }
    }
  }

  void write(Value value, NotifyFn ready_for_next_chunk) override {
    ASSERT(!committed_);
    SimpleCacheSharedPtr cache = cache_;
    if (cache.get() != nullptr && cache->isHealthy()) {
      if (ready_for_next_chunk == nullptr) { // Final chunk
        if (value_.get() == nullptr) {
          value_ = value;
        } else {
          absl::StrAppend(&value_->value_, value->value_);
          value_->timestamp_ = value->timestamp_;
        }
        committed_ = true;
        cache->insertHelper(key_, value_);
      } else {
        if (value_.get() == nullptr) {
          value_ = std::make_shared<ValueStruct>();
        }
        absl::StrAppend(&value_->value_, value->value_);
        ready_for_next_chunk(true);
      }
    }
  }

  const std::string& key() const { return key_; }

private:
  Value value_;
  std::string key_;
  SimpleCacheSharedPtr cache_;
  bool committed_ = false;
};

LookupContextPtr SimpleCache::lookup(const Descriptor& descriptor) {
  return std::make_unique<SimpleLookupContext>(descriptor, self<SimpleCache>());
}

void SimpleCache::lookupHelper(const std::string& key, DataReceiverFn receiver) {
  Value value;
  auto status = DataStatus::NotFound;
  {
    Thread::LockGuard lock(mutex_);
    auto iter = map_.find(key);
    if (iter != map_.end()) {
      value = iter->second;
      if (value.get() == nullptr) {
        // The map entry was present, but the Value pointer contained null.
        // This indicates an insert is in progress.
        status = DataStatus::InsertInProgress;
      } else {
        status = DataStatus::LastChunk;
      }
    }
  }
  // Release the lock before calling the receiver, retaining
  // access the ref-counted value, even if it’s evicted by another
  // thread immediately after releasing the lock.
  receiver(status, value);
}

InsertContextPtr SimpleCache::insert(const Descriptor& descriptor) {
  auto context = std::make_unique<SimpleInsertContext>(descriptor, self<SimpleCache>());
  Value value;
  {
    Thread::LockGuard lock(mutex_);
    map_[context->key()] = value; // Empty value indicates InsertInProgress.
  }
  return context;
}

void SimpleCache::remove(const Descriptor& descriptor, NotifyFn confirm_fn) {
  removeHelper(std::string(descriptor.key()));
  if (confirm_fn != nullptr) {
    confirm_fn(true);
  }
}

void SimpleCache::removeHelper(const std::string& key) {
  Thread::LockGuard lock(mutex_);
  map_.erase(key);
}

// Called by cache user for each chunk to insert into a key.
void SimpleCache::insertHelper(const std::string& key, Value value) {
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
