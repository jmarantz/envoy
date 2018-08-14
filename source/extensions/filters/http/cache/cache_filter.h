#pragma once

#include <queue>

#include "envoy/config/filter/http/cache/v2/cache.pb.h"
#include "envoy/http/filter.h"
#include "envoy/http/header_map.h"
#include "envoy/json/json_object.h"
#include "envoy/runtime/runtime.h"
#include "envoy/stats/stats_macros.h"

#include "common/buffer/buffer_impl.h"
#include "common/compressor/zlib_compressor_impl.h"
#include "common/http/header_map_impl.h"
#include "common/json/config_schemas.h"
#include "common/json/json_validator.h"
#include "common/protobuf/protobuf.h"

#include "extensions/cache/cache.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Cache {

/**
 * All cache filter stats. @see stats_macros.h.
 */
// clang-format off
#define ALL_CACHE_STATS(COUNTER)     \
  COUNTER(cacheable)                \
  COUNTER(uncacheable)              \
  COUNTER(hits)                     \
  COUNTER(inserts)                  \
  COUNTER(misses)                   \
  COUNTER(total_inserted_bytes)     \
  COUNTER(total_cached_bytes)       \
// clang-format on

/**
 * Struct definition for cache stats. @see stats_macros.h
 */
struct CacheStats {
  ALL_CACHE_STATS(GENERATE_COUNTER_STRUCT)
};

/**
 * Configuration for the cache filter.
 */
class CacheFilterConfig {

public:
  CacheFilterConfig(const envoy::config::filter::http::cache::v2::Cache& cache,
                   const std::string& stats_prefix,
                   Stats::Scope& scope, Runtime::Loader& runtime);

  Runtime::Loader& runtime() { return runtime_; }
  CacheStats& stats() { return stats_; }

private:
  static CacheStats generateStats(const std::string& prefix, Stats::Scope& scope) {
    return CacheStats{ALL_CACHE_STATS(POOL_COUNTER_PREFIX(scope, prefix))};
  }

  CacheStats stats_;
  Runtime::Loader& runtime_;
};
typedef std::shared_ptr<CacheFilterConfig> CacheFilterConfigSharedPtr;

/**
 * A filter that compresses data dispatched from the upstream upon client request.
 */
class CacheFilter : public Http::StreamFilter {
public:
  CacheFilter(const CacheFilterConfigSharedPtr& config, const Envoy::Cache::CacheSharedPtr& cache,
              MonotonicTimeSource& time_source);

  // Http::StreamFilterBase
  void onDestroy() override{};

  // Http::StreamDecoderFilter
  Http::FilterHeadersStatus decodeHeaders(Http::HeaderMap& headers, bool end_stream) override;
  Http::FilterDataStatus decodeData(Buffer::Instance&, bool) override {
    return Http::FilterDataStatus::Continue;
  }
  Http::FilterTrailersStatus decodeTrailers(Http::HeaderMap&) override {
    return Http::FilterTrailersStatus::Continue;
  }
  void setDecoderFilterCallbacks(Http::StreamDecoderFilterCallbacks& callbacks) override {
    decoder_callbacks_ = &callbacks;
  };

  // Http::StreamEncoderFilter
  Http::FilterHeadersStatus encode100ContinueHeaders(Http::HeaderMap&) override {
    return Http::FilterHeadersStatus::Continue;
  }
  Http::FilterHeadersStatus encodeHeaders(Http::HeaderMap& headers, bool end_stream) override;
  Http::FilterDataStatus encodeData(Buffer::Instance& buffer, bool end_stream) override;
  Http::FilterTrailersStatus encodeTrailers(Http::HeaderMap&) override;
  void setEncoderFilterCallbacks(Http::StreamEncoderFilterCallbacks& callbacks) override {
    encoder_callbacks_ = &callbacks;
  }

private:
  // TODO(gsagula): This is here temporarily and just to facilitate testing. Ideally all
  // the logic in these private member functions would be availale in another class.
  friend class CacheFilterTest;

  bool isCacheableRequest(Http::HeaderMap& headers) const;
  bool isCacheableResponse(Http::HeaderMap& headers) const;
  void readChunkFromCache();
  Envoy::Cache::ReceiverStatus sendDataDownstream(const Envoy::Cache::Value& value, bool end_stream);
  std::string serializeHeaders(Http::HeaderMap& headers);
  void readyForNextInsertionChunk(bool ok);
  void writeChunk(std::string&& chunk);

  /*
  bool hasCacheControlNoTransform(Http::HeaderMap& headers) const;
  bool isAcceptEncodingAllowed(Http::HeaderMap& headers) const;
  bool isContentTypeAllowed(Http::HeaderMap& headers) const;
  bool isEtagAllowed(Http::HeaderMap& headers) const;
  bool isMinimumContentLength(Http::HeaderMap& headers) const;
  bool isTransferEncodingAllowed(Http::HeaderMap& headers) const;

  void sanitizeEtagHeader(Http::HeaderMap& headers);
  void insertVaryHeader(Http::HeaderMap& headers);

  bool skip_compression_;
  Buffer::OwnedImpl compressed_data_;
  Compressor::ZlibCompressorImpl compressor_;
  */
  CacheFilterConfigSharedPtr config_;

  Http::StreamDecoderFilterCallbacks* decoder_callbacks_{nullptr};
  Http::StreamEncoderFilterCallbacks* encoder_callbacks_{nullptr};

  Envoy::Cache::CacheSharedPtr cache_;
  MonotonicTimeSource& time_source_;
  MonotonicTime current_time_;
  bool enable_cache_fill_ = false;
  bool ready_for_next_insertion_chunk_ = false;
  bool end_insertion_stream_ = false;
  Envoy::Cache::LookupContextPtr lookup_;
  Envoy::Cache::InsertContextPtr insert_;
  std::queue<std::string> buffer_;
};

} // namespace Cache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
