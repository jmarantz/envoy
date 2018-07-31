#include "common/cache/simple_cache.h"

#include "common/cache/cache.h"
#include "common/common/lock_guard.h"

#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"

namespace Envoy {
namespace Cache {

class SimpleLookupContext : public LookupContext {
public:
  SimpleLookupContext(const Key& key, BackendSharedPtr cache)
      : key_str_(std::string(key.key_)),
        split_(key.attributes_.find("split") != key.attributes_.end()), cache_(cache) {}

  void read(DataReceiverFn receiver) override {
    BackendSharedPtr cache = cache_;
    if (cache.get() != nullptr) {
      cache = cache->self();
      if (cache.get() != nullptr) {
        if (!split_) {
          static_cast<SimpleCache*>(cache.get())->lookupHelper(key_str_, receiver);
          return;
        } else if (residual_value_.get() == nullptr) {
          Value value;
          static_cast<SimpleCache*>(cache.get())
              ->lookupHelper(key_str_, [&value, this](DataStatus s, const Value& v) {
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
    }
    receiver(DataStatus::NotFound, Value());
  }

private:
  std::string key_str_;
  bool split_;
  BackendSharedPtr cache_;
  Value residual_value_;
  DataStatus residual_status_;
};

class SimpleInsertContext : public InsertContext {
public:
  SimpleInsertContext(const Key& key, BackendSharedPtr cache)
      : key_str_(std::string(key.key_)), cache_(cache) {}

  virtual ~SimpleInsertContext() {
    // If a live cache has an uncommitted write, remove the 'in-progress' entry.
    if (!committed_) {
      BackendSharedPtr cache = cache_;
      if (cache.get() != nullptr) {
        cache = cache->self();
        if (cache.get() != nullptr) {
          static_cast<SimpleCache*>(cache.get())->removeHelper(key_str_);
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
          static_cast<SimpleCache*>(cache.get())->insertHelper(key_str_, value_);
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

  const std::string& key_str() const { return key_str_; }

private:
  Value value_;
  std::string key_str_;
  BackendSharedPtr cache_;
  bool committed_ = false;
};

LookupContextPtr SimpleCache::lookup(const Key& key) {
  return std::make_unique<SimpleLookupContext>(key, self());
}

void SimpleCache::lookupHelper(const std::string& key_str, DataReceiverFn receiver) {
  Value value;
  auto status = DataStatus::NotFound;
  {
    Thread::LockGuard lock(mutex_);
    auto iter = map_.find(key_str);
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

InsertContextPtr SimpleCache::insert(const Key& key) {
  auto context = std::make_unique<SimpleInsertContext>(key, self());
  Value value;
  {
    Thread::LockGuard lock(mutex_);
    map_[context->key_str()] = value; // Empty value indicates InsertInProgress.
  }
  return context;
}

void SimpleCache::remove(const Key& key, NotifyFn confirm_fn) {
  removeHelper(std::string(key.key_));
  if (confirm_fn != nullptr) {
    confirm_fn(true);
  }
}

void SimpleCache::removeHelper(const std::string& key_str) {
  Thread::LockGuard lock(mutex_);
  map_.erase(key_str);
}

// Called by cache user for each chunk to insert into a key.
void SimpleCache::insertHelper(const std::string& key_str, Value value) {
  // Only lock the cache and write the map when we have the complete value.
  Thread::LockGuard lock(mutex_);
  map_[key_str] = value;
}

CacheInfo SimpleCache::cacheInfo() const {
  CacheInfo cache_info;
  cache_info.chunk_size_bytes_ = 100 * 000;
  cache_info.max_size_bytes_ = 10 * 1000 * 1000;
  return cache_info;
}

} // namespace Cache
} // namespace Envoy
