#include "extensions/filters/http/cache/cache_filter.h"

#include "common/common/macros.h"
#include "extensions/cache/cache.h"
#include "extensions/cache/simple_cache.h"

#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Cache {

CacheFilterConfig::CacheFilterConfig(const envoy::config::filter::http::cache::v2::Cache& /*cache*/,
                                     const std::string& stats_prefix, Stats::Scope& scope,
                                     Runtime::Loader& runtime)
    : stats_(generateStats(stats_prefix + "cache.", scope)), runtime_(runtime) {}

CacheFilter::CacheFilter(const CacheFilterConfigSharedPtr& config,
                         const Envoy::Cache::CacheSharedPtr& cache, MonotonicTimeSource& time_source)
    : config_(config),
      cache_(cache),
      time_source_(time_source) {
    }

Http::FilterHeadersStatus CacheFilter::decodeHeaders(Http::HeaderMap& headers, bool) {
  if (config_->runtime().snapshot().featureEnabled("cache.filter_enabled", 100) &&
      isCacheableRequest(headers)) {
    current_time_ = time_source_.currentTime();;
    Envoy::Cache::Descriptor desc(headers.Path()->value().getStringView(), current_time_);
    lookup_ = cache_->lookup(desc);
    readChunkFromCache();
    return Http::FilterHeadersStatus::StopIteration;
  }
  return Http::FilterHeadersStatus::Continue;
}

void CacheFilter::readChunkFromCache() {
  lookup_->read(
      [this](Envoy::Cache::DataStatus status, const Envoy::Cache::Value& value)
      -> Envoy::Cache::ReceiverStatus {
        switch (status) {
          case Envoy::Cache::DataStatus::NotFound:
            enable_cache_fill_ = true;
            FALLTHRU;
          case Envoy::Cache::DataStatus::Error:
          case Envoy::Cache::DataStatus::InsertInProgress:
            decoder_callbacks_->continueDecoding();
            return Envoy::Cache::ReceiverStatus::Ok;
          case Envoy::Cache::DataStatus::ChunksImminent:
          case Envoy::Cache::DataStatus::ChunksPending: {
            Envoy::Cache::ReceiverStatus status = sendDataDownstream(value, false);
            readChunkFromCache();
            return status;
          }
          case Envoy::Cache::DataStatus::LastChunk: {
            return sendDataDownstream(value, true);
          }
        }
      });
}

Envoy::Cache::ReceiverStatus CacheFilter::sendDataDownstream(
    const Envoy::Cache::Value& value, bool end_stream) {
  // TODO(jmarantz): think about avoiding this string copy, possibly
  // by using Buffer::Instance as part of Envoy::Cache::ValueStruct
  // rather tha a std::string. The drawback is that for an in-memory
  // cache, we'd have to copy the data anyway as downstream filters
  // might mutate the buffer during encoding.
  Buffer::OwnedImpl data(value->value_);
  switch (encodeData(data, end_stream)) {
    case Http::FilterDataStatus::Continue:
      return Envoy::Cache::ReceiverStatus::Ok;
    case Http::FilterDataStatus::StopIterationAndBuffer:
    case Http::FilterDataStatus::StopIterationAndWatermark:
    case Http::FilterDataStatus::StopIterationNoBuffer:
      break;
  }
  return Envoy::Cache::ReceiverStatus::Invalid;
}

Http::FilterHeadersStatus CacheFilter::encodeHeaders(Http::HeaderMap& headers, bool end_stream) {
  if (enable_cache_fill_ && !end_stream && isCacheableResponse(headers)) {
    ASSERT(config_->runtime().snapshot().featureEnabled("cache.filter_enabled", 100));
    // Should we just remember the timestamp from decode() rather than pulling it again?
    Envoy::Cache::Descriptor desc(headers.Path()->value().getStringView(), current_time_);
    insert_ = cache_->insert(std::move(desc));
    writeChunk(serializeHeaders(headers));
  }
  return Http::FilterHeadersStatus::Continue;
}

std::string CacheFilter::serializeHeaders(Http::HeaderMap& headers) {
  // TODO(jmarantz): switch to protobuf. Note that if there is a : in the header this
  // fails, but that's OK for a prototype.
  std::string out;
  headers.iterate([](const Http::HeaderEntry& header, void* context) -> Http::HeaderMap::Iterate {
                    std::string* out = static_cast<std::string*>(context);
                    absl::StrAppend(out, header.key().getStringView(), ": ",
                                    header.value().getStringView(), "\n");
                    return Http::HeaderMap::Iterate::Continue;
                        }, &out);
  // Add an extra newline to identify the gap to main content.
  absl::StrAppend(&out, "\n");
  return out;
}

Http::FilterDataStatus CacheFilter::encodeData(Buffer::Instance& data, bool end_stream) {
  if (insert_.get() != nullptr) {
    end_insertion_stream_ = end_stream;
    if (ready_for_next_insertion_chunk_) {
      ASSERT(buffer_.empty());
      ready_for_next_insertion_chunk_= false;
      // TODO(jmarantz): this very non-performant. I think we should probably hold onto
      // the Buffer::Instance and stream the data out to the cache at a pace dictated
      // to us by the cache, but I don't see a mechanism right now to take ownership of
      // data past the end of this function. So for now, getting a functional prototype
      // working that just serializes the data into a string and writes it all at once
      // is most expedient.
      writeChunk(data.toString());
    } else {
      // The cache can't receive the data as fast as our upstream can send it, so
      // try to push back, but remember
      buffer_.push(data.toString());
      return Http::FilterDataStatus::StopIterationAndWatermark;
    }
  }
  return Http::FilterDataStatus::Continue;
}

void CacheFilter::readyForNextInsertionChunk(bool ok) {
  if (ok) {
    ASSERT(!ready_for_next_insertion_chunk_);
    if (buffer_.empty()) {
      ready_for_next_insertion_chunk_= true;
    } else {
      std::string value;
      value.swap(buffer_.front());
      buffer_.pop();
      writeChunk(std::move(value));
    }
  } else {
    // Cache has aborted the operation. Drop the insertion context and buffers.
    insert_.reset();
    std::queue<std::string> temp;
    temp.swap(buffer_);  // std::queue lacks clear().
  }
}

void CacheFilter::writeChunk(std::string&& chunk) {
  Envoy::Cache::Value value = Envoy::Cache::makeValue();
  value->value_ = std::move(chunk);
  value->timestamp_ = current_time_;
  if (end_insertion_stream_ && buffer_.empty()) {
    insert_->write(value, nullptr);
    insert_.reset();
  } else {
    insert_->write(value, [this](bool ok) { readyForNextInsertionChunk(ok); });
  }
}

bool CacheFilter::isCacheableRequest(Http::HeaderMap& headers) const {
  const Http::HeaderEntry* method = headers.Method();
  return ((method != nullptr) && method->value().getStringView() == "GET");
}

bool CacheFilter::isCacheableResponse(Http::HeaderMap& headers) const {
  const Http::HeaderEntry* cache_control = headers.CacheControl();

  // Cache header analysis is a complex topic. A better start at this can
  // be found in
  // https://github.com/apache/incubator-pagespeed-mod/blob/master/pagespeed/kernel/http/caching_headers.h
  // but for now we'll just see if cache-control is present and does not contain "private".

  bool ok = (cache_control != nullptr) &&
            (cache_control->value().getStringView().find("private") == absl::string_view::npos);
  if (ok) {
    config_->stats().cacheable_.inc();
  } else {
    config_->stats().uncacheable_.inc();
  }
  return ok;
}

} // namespace Cache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
