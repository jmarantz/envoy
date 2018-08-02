#include "common/cache/cache.h"

namespace Envoy {
namespace Cache {

CacheInterfaceSharedPtr::CacheInterfaceSharedPtr() : self_(this) {}

CacheInterfaceSharedPtr::~CacheInterfaceSharedPtr() { ASSERT(self_.get() == nullptr); }

LookupContextVec CacheInterfaceSharedPtr::multiLookup(const DescriptorVec& descriptors) {
  LookupContextVec lookups;
  lookups.reserve(descriptors.size());
  for (const auto& desc : descriptors) {
    lookups.push_back(lookup(desc));
  }
  return lookups;
}

void CacheInterfaceSharedPtr::shutdown(NotifyFn done) {
  ASSERT(self_.get() != nullptr);
  self_ = nullptr;
  if (done) {
    done(true);
  }
}

bool ValidStatus(DataStatus status) {
  return status == DataStatus::ChunksImminent || status == DataStatus::ChunksPending ||
         status == DataStatus::LastChunk;
}

bool TerminalStatus(DataStatus status) {
  return !ValidStatus(status) || status == DataStatus::LastChunk;
}

InsertContext::~InsertContext() {}
LookupContext::~LookupContext() {}

Value makeValue() { return std::make_shared<ValueStruct>(); }

} // namespace Cache
} // namespace Envoy
