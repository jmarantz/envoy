#include "common/cache/cache.h"

namespace Envoy {
namespace Cache {

CacheInterface::CacheInterface() : self_(this) {}

CacheInterface::~CacheInterface() { ASSERT(self_.get() == nullptr); }

LookupContextVec CacheInterface::multiLookup(const DescriptorVec& descriptors) {
  LookupContextVec lookups;
  lookups.reserve(descriptors.size());
  for (const auto& desc : descriptors) {
    lookups.push_back(lookup(desc));
  }
  return lookups;
}

void CacheInterface::shutdown(NotifyFn done) {
  ASSERT(self_.get() != nullptr);
  self_ = nullptr;
  if (done) {
    done(true);
  }
}

bool validStatus(DataStatus status) {
  return status == DataStatus::ChunksImminent || status == DataStatus::ChunksPending ||
         status == DataStatus::LastChunk;
}

bool terminalStatus(DataStatus status) {
  return !validStatus(status) || status == DataStatus::LastChunk;
}

InsertContext::~InsertContext() {}
LookupContext::~LookupContext() {}

Value makeValue() { return std::make_shared<ValueStruct>(); }

bool Descriptor::findAttribute(absl::string_view name, absl::string_view& value) const {
  for (const Attribute& attr : attributes_) {
    if (attr.name_ == name) {
      value = attr.value_;
      return true;
    }
  }
  return false;
}

bool Descriptor::hasAttribute(absl::string_view name) const {
  return std::find_if(attributes_.begin(), attributes_.end(), [name](const Attribute& attr) {
           return (attr.name_ == name);
         }) != attributes_.end();
}

} // namespace Cache
} // namespace Envoy
