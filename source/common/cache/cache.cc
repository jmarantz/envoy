#include "common/cache/cache.h"

namespace Envoy {
namespace Cache {

Backend::Backend() : self_(this) {}

Backend::~Backend() { ASSERT(self_.get() == nullptr); }

void Backend::multiLookup(const MultiLookupRequest&) { ASSERT(false); }

bool ValidStatus(DataStatus status) {
  return status == DataStatus::ChunksImminent || status == DataStatus::ChunksPending ||
         status == DataStatus::LastChunk;
}

bool TerminalStatus(DataStatus status) {
  return !ValidStatus(status) || status == DataStatus::LastChunk;
}

LookupContext::~LookupContext() {}

} // namespace Cache
} // namespace Envoy
