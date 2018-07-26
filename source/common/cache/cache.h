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

using AttributeMap = std::map<std::string, std::string>;
struct Key {
  bool operator<(const Key& that) const { return key_ < that.key_; }
  std::string key_;
  AttributeMap attributes_;
};

// Status returned from a receiver function, which is used for both lookups and insertions.
enum class ReceiverStatus {
  Ok,      // The data was received and we are ready for the next chunk.
  Abort,   // The receiver is no longer interested in the data.
  Invalid, // The data is no longer valid and should be removed. The caller will need to
            // request this response from another cache or from a backend.
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

enum class RemoveStatus { kRemoved, kError };
using DataReceiverFn = std::function<ReceiverStatus(DataStatus, const Value&)>;
using KeyReceiverFn = std::pair<Key, DataReceiverFn>;
using MultiLookupRequest = std::vector<KeyReceiverFn>;
using NotifyFn = std::function<void(bool)>;

// Returns the ideal chunk-size of data stored in the cache. Items larger than
// this should be stored in chunks, facilitating pausable downloads and video
// streaming. It may be possible to store larger items in one chunk, but
// sufficiently large chunks may be dropped on insert. Note that the chunking
// is purely under control of the application, including encoding the range into
// the key, and this size is offered only as a hint for optimal performance.
// Example usages: memcached (in some versions) had a maximum size of 1M and
// so chunked storage strategies were required. Shared-memory caches may, for
// simplicity, allocate fixed size buffers for cache values, thus requiring
// chunking larger values.
//
// Chunking is not managed by the caching backends themselves because
// they don’t have a way to induce a fill for any missing chunks.
struct CacheInfo {
  size_t chunk_size_bytes_; // Optimum size for range-requests.
  size_t max_size_bytes_;   // Maximum permissible size for single chunks.
};

class Backend {
public:
  virtual ~Backend();

  // Streams the cached payload stored at key into data_receiver. If data_receiver
  // wants to cancel the lookup mid-stream, it can return ReceiverStatus::Abort.
  // Note that a lookup cannot be considered complete until DataStatus::LastChunk
  // is passed into the receiver. At any point the receiver can return
  // DataStatus::Invalid, at which point a 2-level cache can discard its L1
  // result and try the L2. This is useful in HTTP caches with multiple distinct
  // L1 caches all sharing an L2. If there is an expired entry in an L1, an
  // Invalid signal triggers the writethrough cache to check the L2.
  //
  // Note that the key includes an AttributeMap, where range requests and variants can be
  // encoded by appropriate wrappers.
  //
  // Usage:
  //   cache->Lookup(key, [key](Cache::DataStatus status, const Value& data)
  //     -> Cache::ReceiverStatus {
  //       // Here we can stream data out from the cache to a socket, and return
  //       // Cache::ReceiverStatus::kAbort if there was a problem, so the cache
  //       // can terminate the stream.
  //       std::cout << “Lookup(“ << key << “ << “): “ << status << “; “ << data;
  //       return Cache::ReceiverStatus::Ok;
  //     });
  virtual void lookup(const Key& key, DataReceiverFn data_receiver) = 0;

  // Performs multiple lookups in parallel. This is equivalent to calling lookup
  // for each item in the request vector, but provides an opportunity for networked
  // caches to batch multiple requests into a single RPC. Note that each key gets
  // its own data-receiver function, and there is no single notification for
  // completing all the lookups.
  virtual void multiLookup(const MultiLookupRequest& req);

  // Initiates an insertion at key. Any previous value is discarded
  // upon calling this function. The insertion is streamed into the
  // cache, and during insertion, any lookups on that key will return
  // DataStatus::InsertInProgress.  The cache backend provides a
  // DataReceiverFn to insert_fn so the data can be streamed in. The
  // insertion logic can cancel the insertion at any time during the
  // process by calling the receiver function with DataStatus::Error,
  // in which case the key will be left unset in the cache.  The cache
  // backend can also signal to the inserter that it’s no longer
  // accepting the insertion by returning ReceiverStatus::Abort from
  // the receiver.
  //
  // Usage:
  //   cache->insert(key, [](Cache::DataReceiverFn inserter) {
  //     // Write two chunks to into the cache. Only write the second chunk if the
  //     // cache was able to injest the first chunk. Note that no status is returned
  //     // by insert() because even if insert() succeeds, there is no guarantee a
  //     // subsequent lookup would succeed.
  //     if (inserter(Cache::DataStatus::kChunksImminent, “chunk1”) ==
  //                  Cache::ReceiverStatus::kOk) {
  //       inserter(Cache::DataStatus::LastChunk, “chunk2”)’;
  //     }
  //   });
  virtual DataReceiverFn insert(const Key& key) = 0;

  // Removes a cache key. If a confirmation callback is provided, it will
  // be called once the deletion is complete.
  virtual void remove(const Key& key, NotifyFn confirm_fn) = 0;

  // Initiates a cache shutdown.
  //   1. Puts the cache into a lameduck mode, where every request immediately fails.
  //   2. Deferring until all outstanding requests are retired
  //   3. Calling the done callback if provided
  //   4. Self-deleting.
  // It is a programming error to delete a cache with outstanding requests pending.
  virtual void Shutdown(NotifyFn done) {
    ASSERT(self_.get() != nullptr);
    self_ = nullptr;
    if (done) {
      done(true);
    }
  }

  // Returns whether the cache is in a healthy state. A unhealthy cache can
  // respond immediately to lookups by indicating DataStatus::Error, and
  // to inserts with RemoveStatus::Error. However, some operations may benefit
  // from knowing that caching is impossible before they even begin. For example,
  // a costly Brotli compression filter may disable itself when the result can’t
  // be cached due to transient issues, such as a costly Brotli-compression run
  // per-request.
  virtual bool IsHealthy() const { return self_.get() != nullptr; }

  virtual CacheInfo cacheInfo() const = 0;

  std::shared_ptr<Backend> self() { return self_; }

protected:
  Backend();

private:
  std::shared_ptr<Backend> self_; // Cleared on shutdown.
};

using BackendSharedPtr = std::shared_ptr<Backend>;

// Insertion context manages the lifetime of an insertion. Client code wishing
// to insert something into a cache can use this to stream data into a cache.
// An insertion context is returned by Backend::insert(). Clients should only
// present data to the cache when the ready() function passed into the context
// is called. The data presented should be bounded in size.
class InsertionContext {
 public:
  // If ready() is called with 'true', it's time for the client to write a new chunk
  // to the cache, abort it, or finalize.  If ready() is called with 'false', the
  // operation is aborted by the cache, and the client can free any contextual information
  // and stop streaming.
  InsertionContext(NotifyFn ready);
  virtual ~InsertionContext();

  bool write(Value chunk);
  void finalize();
  void abort();

 private:
  BackendSharedPtr backend_;
};

// Lookup context manages the lifetime of a lookup.
class LookupContext {
 public:
  LookupContext(NotifyFn ready);
  virtual ~LookupContext();

  DataStatus read(Value& value);

 private:
  BackendSharedPtr backend_;
};

} // namespace Cache
} // namespace Envoy
