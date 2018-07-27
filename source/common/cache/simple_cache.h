#pragma once

#include "common/cache/cache.h"
#include "common/common/thread.h"

namespace Envoy {
namespace Cache {

// Example cache backend that never evicts. It blocks on a mutex
// during operations, but this can be wrapped with a multi-thread
// dispatcher to avoid contention.
class SimpleCache : public Backend {
public:
  LookupContextPtr lookup(const Key& key) override;
  InsertContextPtr insert(const Key& key) override;
  void remove(const Key& key, NotifyFn confirm_fn) override;
  CacheInfo cacheInfo() const override;

private:
  friend class SimpleInsertContext;
  friend class SimpleLookupContext;

  // Called by SimpleLookupContext on each chunk.
  void lookupHelper(const Key& key, DataReceiverFn receiver);

  // Called by SimpleInsertContext when insertion is finalized.
  void insertHelper(const Key& key, Value value);

  std::map<Key, Value> map_ GUARDED_BY(mutex_);
  Thread::MutexBasicLockable mutex_;
};

} // namespace Cache
} // namespace Envoy
