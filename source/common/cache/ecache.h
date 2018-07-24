#pragma once

#include <map>
#include <string>

#include "envoy/common/time.h"

#include "common/common/assert.h"
#include "common/common/utility.h"

namespace Envoy {

namespace ecache {

struct ValueStruct {
  MonotonicTime timestamp;
  std::string value;
};
using Value = std::shared_ptr<ValueStruct>;

using AttributeMap = std::map<std::string, std::string>;
using Key = std::pair<std::string, AttributeMap>;

// Status returned from a receiver function, which is used for both lookups and insertions.
enum class ReceiverStatus {
  kOk,       // The data was received and we are ready for the next chunk.
  kAbort,    // The receiver is no longer interested in the data.
  kInvalid,  // The data is no longer valid and should be deleted. The caller will need to
             // request this response from another cache or from a backend.
};

// Status passed to a receiver function.
enum class DataStatus {
  kNotFound,        // The value was not found, or has become invalid during streaming.
  kChunksImminent,  // Another chunk of data will immediately follow.
  kChunksPending,   // Another chunk of data will eventually follow, perhaps after a delay.
  kLastChunk,       // The current chunk is the last one.
  kError,           // An error has occurred during streaming; detailed in the data.
};

bool ValidStatus(DataStatus status) {
  return status == DataStatus::kChunksImminent || status == DataStatus::kChunksPending || 
    status == DataStatus::kLastChunk;
}

enum class DeleteStatus { kDeleted, kError };
using DataReceiverFn = std::function<ReceiverStatus(DataStatus, const Value&)>;
using KeyReceiverFn = std::pair<Key, DataReceiverFn>;
using MultiLookupRequest = std::vector<KeyReceiverFn>;
using NotifyFn = std::function<void(bool)>;

class Backend {
public:
  virtual ~Backend() { ASSERT(self_.get() == nullptr); }

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
  //   cache->Lookup(key, [key](ecache::DataStatus status, const Value& data)
  //     -> ecache::ReceiverStatus {
  //       // Here we can stream data out from the cache to a socket, and return
  //       // ecache::ReceiverStatus::kAbort if there was a problem, so the cache
  //       // can terminate the stream.
  //       std::cout << “Lookup(“ << key << “ << “): “ << status << “; “ << data;
  //       return ecache::ReceiverStatus::Ok;
  //     });
  virtual void Lookup(const Key& key, DataReceiverFn data_receiver) = 0;

  // Performs multiple lookups in parallel. This is equivalent to calling lookup
  // for each item in the request vector, but provides an opportunity for networked
  // caches to batch multiple requests into a single RPC. Note that each key gets
  // its own data-receiver function, and there is no single notification for 
  // completing all the lookups.
  virtual void multiLookup(const MultiLookupRequest& req) = 0;

  // Initiates an insertion at key. Any previous value is discarded upon
  // calling this function. The insertion is streamed into the cache, and
  // during insertion, any lookups on that key will return DataStatus::NotFound.
  // The cache backend provides a DataReceiverFn to insert_fn so the
  // data can be streamed in. The insertion logic can cancel the insertion
  // at any time during the process by calling the receiver function with
  // DataStatus::Error, in which case the key will be left unset in the cache.
  // The cache backend can also signal to the inserter that it’s no
  // longer accepting the insertion by returning ReceiverStatus::Abort from the
  // receiver.
  //
  // Usage:
  //   cache->Insert(key, [](ecache::DataReceiverFn inserter) {
  //     // Write two chunks to into the cache. Only write the second chunk if the
  //     // cache was able to injest the first chunk. Note that no status is returned
  //     // by Insert() because even if Insert() succeeds, there is no guarantee a
  //     // subsequent lookup would succeed.
  //     if (inserter(ecache::DataStatus::kChunksImminent, “chunk1”) == 
  //                  ecache::ReceiverStatus::kOk) {
  //       inserter(ecache::DataStatus::LastChunk, “chunk2”)’;
  //     }
  //   });
  virtual DataReceiverFn Insert(const Key& key) = 0;

  // Removes a cache key. If a confirmation callback is provided, it will
  // be called once the deletion is complete.
  virtual void Delete(const Key& key, NotifyFn confirm_fn) = 0;

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
  // to inserts with DeleteStatus::Error. However, some operations may benefit
  // from knowing that caching is impossible before they even begin. For example,
  // a costly Brotli compression filter may disable itself when the result can’t
  // be cached due to transient issues, such as a costly Brotli-compression run 
  // per-request.
  virtual bool isHealthy() const = 0;

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
  virtual size_t ChunkSizeBytes() const = 0;

  std::shared_ptr<Backend> self() { return self_; }

 protected:
  Backend() : self_(this) {}

 private:
  std::shared_ptr<Backend> self_; // Cleared on shutdown.
};

using BackedPtr = std::shared_ptr<Backend>;

}  // namespace ecache
}  // namespace Envoy
