#include "common/cache/cache.h"

namespace Envoy {
namespace Cache {

Backend::Backend() : self_(this) {}

Backend::~Backend() { ASSERT(self_.get() == nullptr); }

void Backend::multiLookup(const MultiLookupRequest&) { ASSERT(false); }

bool ValidStatus(DataStatus status) {
  return status == DataStatus::kChunksImminent || status == DataStatus::kChunksPending ||
         status == DataStatus::kLastChunk;
}

} // namespace Cache
} // namespace Envoy
