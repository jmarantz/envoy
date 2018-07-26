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
  void lookup(const Key& key, DataReceiverFn receiver) override;
  DataReceiverFn insert(const Key& key) override;
  void remove(const Key& key, NotifyFn confirm_fn) override;
  CacheInfo cacheInfo() const override;

 private:
  // Called by cache user for each chunk to insert into a key.
  ReceiverStatus InsertHelper(DataStatus status, Key key, Value value, const Value& chunk);

  std::map<Key, Value> map_ GUARDED_BY(mutex_);
  Thread::MutexBasicLockable mutex_;
};

} // namespace Cache
} // namespace Envoy
