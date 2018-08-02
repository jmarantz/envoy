#pragma once

#include <map>
#include <string>

#include "envoy/common/time.h"

#include "common/common/assert.h"
#include "common/common/utility.h"

namespace Envoy {
namespace Cache {

struct ValueStruct {
  std::string value_;
  MonotonicTime timestamp_;
};
using Value = std::shared_ptr<ValueStruct>;
Value makeValue();

// All required strings in the attribute-map and key are copied into the cache
// on API calls, so the caller need only maintain the storage when initiating an
// API.
using AttributeMap = std::unordered_map<absl::string_view, absl::string_view, StringViewHash>;

// A descriptor identifies an item in a cache. The intent is that key_ is used
// for lookup (e.g. in a hash-map). timestamp_ is used for qualification against
// an invalidation set -- the invalidation can be performed by an adapter.
// attributes_ are transformed by adapters to add detail to the key, e.g. for
// range requests or Vary-handling. By the time the physical cache is reached,
// the key_ is all that's required to perform a lookup or insertion.
struct Descriptor {
  absl::string_view key_;
  MonotonicTime timestamp_;
  AttributeMap attributes_;
};

// Status returned from a receiver function, which is used for both lookups and insertions.
enum class ReceiverStatus {
  Ok,      // The data was received and we are ready for the next chunk.
  Invalid, // The data is no longer valid and should be removed. The caller will need to
           // request this response from another cache or from a backend. This is intended
           // to help write-through cache adapters.
};

// Status passed to a receiver function.
enum class DataStatus {
  NotFound,         // The value was not found, or has become invalid during streaming.
  ChunksImminent,   // Another chunk of data will immediately follow.
  ChunksPending,    // Another chunk of data will eventually follow, perhaps after a delay.
  LastChunk,        // The current chunk is the last one.
  InsertInProgress, // An insertion is currently in progress, so the value cannot be read.
  Error,            // An error has occurred during streaming; detailed in the data.
};

bool ValidStatus(DataStatus status);
bool TerminalStatus(DataStatus status);

using DataReceiverFn = std::function<ReceiverStatus(DataStatus, const Value&)>;
using NotifyFn = std::function<void(bool)>;

// Insertion context manages the lifetime of an insertion. Client code wishing
// to insert something into a cache can use this to stream data into a cache.
// An insertion context is returned by CacheInterface::insert(). Clients should only
// present data to the cache when the ready() function passed into the context
// is called. The data presented should be bounded in size.
class InsertContext {
public:
  virtual ~InsertContext();

  // The insertion is streamed into the cache in chunks whose size is determined
  // by the client, but with a pace determined by the cache. To avoid streaming
  // data into cache too fast for the cache to handle, clients should wait for
  // the cache to call ready_for_next_chunk() before streaming the next chunk.
  // Note that the call to write() passes a shared-string, so for an in-memory
  // cache, it's possible to achieve zero-copy insertions.
  //
  // The client can abort the streaming insertion by dropping the
  // InsertContextPtr. A cache can abort the insertion by passing 'false'
  // into ready_for_next_chunk.
  virtual void write(Value chunk, NotifyFn ready_for_next_chunk) = 0;
};

// Statically known information about a cache.
struct CacheInfo {
  size_t chunk_size_bytes_; // Optimum size for range-requests.
  size_t max_size_bytes_;   // Maximum permissible size for single chunks.
};

// Lookup context manages the lifetime of a lookup, helping clients to pull
// data from the cache at pace that works for them. Call read() until the
// receiver receives a DataStatus where TerminalStatus(status) is true (error
// or last chunk). At any time a client can abort in-progress lookup by simply
// dropping the LookupContextPtr.
class LookupContext {
public:
  virtual ~LookupContext();

  // Reads the next chunk from the cache, calling receiver when the chunk is
  // ready. The receiver is called with only one chunk, and the client must
  // call read again to request another chunk. The streaming can be terminated
  // by the client at any time by simply dropping its reference to the
  // LookupContext.
  //
  // Note that small objects may be sent all in one chunk. The chunk-size is
  // fully controlled by the cache implementation. A lookup is considered
  // complete when DataStatus::LastChunk is passed into the receiver. At any
  // point the receiver can return DataStatus::Invalid, at which point a 2-level
  // cache should discard its L1 result and try the L2. This is useful in HTTP
  // caches with multiple distinct L1 caches all sharing an L2. If there is an
  // expired entry in an L1, an Invalid signal triggers the writethrough cache
  // to check the L2.
  virtual void read(DataReceiverFn receiver) = 0;
};

enum class RemoveStatus { kRemoved, kError };
using DescriptorVec = std::vector<Descriptor>;
using InsertContextPtr = std::unique_ptr<InsertContext>;
using LookupContextPtr = std::unique_ptr<LookupContext>;
using LookupContextVec = std::vector<LookupContextPtr>;

class CacheInterface {
public:
  virtual ~CacheInterface();

  // Initiates streaming of cached cached payload stored at key. The client
  // calls LookupContext::read(). Note that the descriptor includes an
  // AttributeMap, where range requests and variants can be encoded by
  // appropriate wrappers.
  virtual LookupContextPtr lookup(const Descriptor& descriptor) = 0;

  // Performs multiple lookups in parallel. This is equivalent to calling lookup
  // for each item in the request vector, but provides an opportunity for
  // networked caches to batch multiple requests into a single RPC. Note that
  // each key gets its own data-receiver function, and there is no single
  // notification for completing all the lookups.
  //
  // The default implementation simply initiates all the lookups
  // in parallel, but implementations can override if it's more efficient
  // for them to batch RPCs to common backends.
  virtual LookupContextVec multiLookup(const DescriptorVec& descriptors);

  // Initiates an insertion at key. Any previous value is immediately
  // discarded when calling this function, and replaced with a
  // sentinal indicating that an insertion is in progress. Lookups
  // made during insertions will yield DataStatus::InsertInProgress.
  virtual InsertContextPtr insert(const Descriptor& descriptor) = 0;

  // Removes a cache key. If a confirmation callback is provided, it will
  // be called once the deletion is complete.
  virtual void remove(const Descriptor& descriptor, NotifyFn confirm_fn) = 0;

  // Initiates a cache shutdown.
  //   1. Puts the cache into a lameduck mode, where every request immediately fails.
  //   2. Deferring until all outstanding requests are retired
  //   3. Calling the done callback if provided
  //   4. Self-deleting.
  // It is a programming error to delete a cache with outstanding requests pending.
  virtual void shutdown(NotifyFn done);

  // Returns whether the cache is in a healthy state. A unhealthy cache can
  // respond immediately to lookups by indicating DataStatus::Error, and
  // to inserts with RemoveStatus::Error. However, some operations may benefit
  // from knowing that caching is impossible before they even begin. For example,
  // a costly Brotli compression filter may disable itself when the result can’t
  // be cached due to transient issues, such as a costly Brotli-compression run
  // per-request.
  virtual bool isHealthy() const { return self_.get() != nullptr; }

  // Returns statically known information about a cache.
  virtual CacheInfo cacheInfo() const = 0;

  // Returns a name describing the cache. Note that for adapaters, the name may
  // be computed from the caches it is constructed with.
  virtual std::string name() const = 0;

  std::shared_ptr<CacheInterface> self() { return self_; }

protected:
  CacheInterface();

private:
  std::shared_ptr<CacheInterface> self_; // Cleared on shutdown.
};

using CacheSharedPtr = std::shared_ptr<CacheInterface>;

} // namespace Cache
} // namespace Envoy
