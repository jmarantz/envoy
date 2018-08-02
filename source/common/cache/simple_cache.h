#pragma once

#include "common/cache/cache.h"
#include "common/common/thread.h"

namespace Envoy {
namespace Cache {

// Example cache backend that never evicts. It blocks on a mutex
// during operations, but this can be wrapped with a multi-thread
// dispatcher to avoid contention.
class SimpleCache : public CacheInterface {
public:
  static CacheSharedPtr make() { return (new SimpleCache)->self<SimpleCache>(); }

  LookupContextPtr lookup(const Descriptor& descriptor) override;
  InsertContextPtr insert(const Descriptor& descriptor) override;
  void remove(const Descriptor& descriptor, NotifyFn confirm_fn) override;
  CacheInfo cacheInfo() const override;
  std::string name() const override { return "SimpleCache"; }

private:
  friend class SimpleInsertContext;
  friend class SimpleLookupContext;

  // Called by SimpleLookupContext on each chunk.
  void lookupHelper(const std::string& descriptor, DataReceiverFn receiver);

  // Called by SimpleInsertContext when insertion is finalized.
  void insertHelper(const std::string& descriptor, Value value);

  void removeHelper(const std::string& descriptor);

  std::map<std::string, Value> map_ GUARDED_BY(mutex_);
  Thread::MutexBasicLockable mutex_;
};

} // namespace Cache
} // namespace Envoy
