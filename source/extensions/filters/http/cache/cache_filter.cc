#include "extensions/filters/http/cache/cache_filter.h"

#include "common/common/macros.h"

#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Cache {

namespace {
// Default zlib memory level.
const uint64_t DefaultMemoryLevel = 5;

// Default and maximum compression window size.
const uint64_t DefaultWindowBits = 12;

// Minimum length of an upstream response that allows compression.
const uint64_t MinimumContentLength = 30;

// When summed to window bits, this sets a cache header and trailer around the compressed data.
const uint64_t CacheHeaderValue = 16;

// Used for verifying accept-encoding values.
const char ZeroQvalueString[] = "q=0";

// Default content types will be used if any is provided by the user.
const std::vector<std::string>& defaultContentEncoding() {
  CONSTRUCT_ON_FIRST_USE(std::vector<std::string>,
                         {"text/html", "text/plain", "text/css", "application/javascript",
                          "application/json", "image/svg+xml", "text/xml",
                          "application/xhtml+xml"});
}

} // namespace

CacheFilterConfig::CacheFilterConfig(const envoy::config::filter::http::cache::v2::Cache& cache,
                                   const std::string& stats_prefix, Stats::Scope& scope,
                                   Runtime::Loader& runtime)
    : compression_level_(compressionLevelEnum(cache.compression_level())),
      compression_strategy_(compressionStrategyEnum(cache.compression_strategy())),
      content_length_(contentLengthUint(cache.content_length().value())),
      memory_level_(memoryLevelUint(cache.memory_level().value())),
      window_bits_(windowBitsUint(cache.window_bits().value())),
      content_type_values_(contentTypeSet(cache.content_type())),
      disable_on_etag_header_(cache.disable_on_etag_header()),
      remove_accept_encoding_header_(cache.remove_accept_encoding_header()),
      stats_(generateStats(stats_prefix + "cache.", scope)), runtime_(runtime) {}

Compressor::ZlibCompressorImpl::CompressionLevel CacheFilterConfig::compressionLevelEnum(
    envoy::config::filter::http::cache::v2::Cache_CompressionLevel_Enum compression_level) {
  switch (compression_level) {
  case envoy::config::filter::http::cache::v2::Cache_CompressionLevel_Enum::
      Cache_CompressionLevel_Enum_BEST:
    return Compressor::ZlibCompressorImpl::CompressionLevel::Best;
  case envoy::config::filter::http::cache::v2::Cache_CompressionLevel_Enum::
      Cache_CompressionLevel_Enum_SPEED:
    return Compressor::ZlibCompressorImpl::CompressionLevel::Speed;
  default:
    return Compressor::ZlibCompressorImpl::CompressionLevel::Standard;
  }
}

Compressor::ZlibCompressorImpl::CompressionStrategy CacheFilterConfig::compressionStrategyEnum(
    envoy::config::filter::http::cache::v2::Cache_CompressionStrategy compression_strategy) {
  switch (compression_strategy) {
  case envoy::config::filter::http::cache::v2::Cache_CompressionStrategy::
      Cache_CompressionStrategy_RLE:
    return Compressor::ZlibCompressorImpl::CompressionStrategy::Rle;
  case envoy::config::filter::http::cache::v2::Cache_CompressionStrategy::
      Cache_CompressionStrategy_FILTERED:
    return Compressor::ZlibCompressorImpl::CompressionStrategy::Filtered;
  case envoy::config::filter::http::cache::v2::Cache_CompressionStrategy::
      Cache_CompressionStrategy_HUFFMAN:
    return Compressor::ZlibCompressorImpl::CompressionStrategy::Huffman;
  default:
    return Compressor::ZlibCompressorImpl::CompressionStrategy::Standard;
  }
}

StringUtil::CaseUnorderedSet CacheFilterConfig::contentTypeSet(
    const Protobuf::RepeatedPtrField<Envoy::ProtobufTypes::String>& types) {
  return types.empty() ? StringUtil::CaseUnorderedSet(defaultContentEncoding().begin(),
                                                      defaultContentEncoding().end())
                       : StringUtil::CaseUnorderedSet(types.cbegin(), types.cend());
}

uint64_t CacheFilterConfig::contentLengthUint(Protobuf::uint32 length) {
  return length >= MinimumContentLength ? length : MinimumContentLength;
}

uint64_t CacheFilterConfig::memoryLevelUint(Protobuf::uint32 level) {
  return level > 0 ? level : DefaultMemoryLevel;
}

uint64_t CacheFilterConfig::windowBitsUint(Protobuf::uint32 window_bits) {
  return (window_bits > 0 ? window_bits : DefaultWindowBits) | CacheHeaderValue;
}

CacheFilter::CacheFilter(const CacheFilterConfigSharedPtr& config)
    : skip_compression_{true}, compressed_data_(), compressor_(), config_(config) {}

Http::FilterHeadersStatus CacheFilter::decodeHeaders(Http::HeaderMap& headers, bool) {
  if (config_->runtime().snapshot().featureEnabled("cache.filter_enabled", 100) &&
      isAcceptEncodingAllowed(headers)) {
    skip_compression_ = false;
    if (config_->removeAcceptEncodingHeader()) {
      headers.removeAcceptEncoding();
    }
  } else {
    config_->stats().not_compressed_.inc();
  }

  return Http::FilterHeadersStatus::Continue;
}

Http::FilterHeadersStatus CacheFilter::encodeHeaders(Http::HeaderMap& headers, bool end_stream) {
  if (!end_stream && !skip_compression_ && isMinimumContentLength(headers) &&
      isContentTypeAllowed(headers) && !hasCacheControlNoTransform(headers) &&
      isEtagAllowed(headers) && isTransferEncodingAllowed(headers) && !headers.ContentEncoding()) {
    sanitizeEtagHeader(headers);
    insertVaryHeader(headers);
    headers.removeContentLength();
    //@ headers.insertContentEncoding().value(Http::Headers::get().ContentEncodingValues.Cache);
    compressor_.init(config_->compressionLevel(), config_->compressionStrategy(),
                     config_->windowBits(), config_->memoryLevel());
    config_->stats().compressed_.inc();
  } else if (!skip_compression_) {
    skip_compression_ = true;
    config_->stats().not_compressed_.inc();
  }
  return Http::FilterHeadersStatus::Continue;
}

Http::FilterDataStatus CacheFilter::encodeData(Buffer::Instance& data, bool end_stream) {
  if (!skip_compression_) {
    config_->stats().total_uncompressed_bytes_.add(data.length());
    compressor_.compress(data, end_stream ? Compressor::State::Finish : Compressor::State::Flush);
    config_->stats().total_compressed_bytes_.add(data.length());
  }
  return Http::FilterDataStatus::Continue;
}

bool CacheFilter::hasCacheControlNoTransform(Http::HeaderMap& headers) const {
  const Http::HeaderEntry* cache_control = headers.CacheControl();
  if (cache_control) {
    return StringUtil::caseFindToken(cache_control->value().c_str(), ",",
                                     Http::Headers::get().CacheControlValues.NoTransform.c_str());
  }

  return false;
}

// TODO(gsagula): Since cache is the only available content-encoding in Envoy at the moment,
// order/priority of preferred server encodings is disregarded (RFC2616-14.3). Replace this
// with a data structure that parses Accept-Encoding values and allows fast lookup of
// key/priority. Also, this should be part of some utility library.
// https://www.w3.org/Protocols/rfc2616/rfc2616-sec14.html
bool CacheFilter::isAcceptEncodingAllowed(Http::HeaderMap& headers) const {
  const Http::HeaderEntry* accept_encoding = headers.AcceptEncoding();

  if (accept_encoding) {
    bool is_wildcard = false; // true if found and not followed by `q=0`.
    for (const auto token : StringUtil::splitToken(headers.AcceptEncoding()->value().c_str(), ",",
                                                   false /* keep_empty */)) {
      const auto value = StringUtil::trim(StringUtil::cropRight(token, ";"));
      const auto q_value = StringUtil::trim(StringUtil::cropLeft(token, ";"));
      // If value is the cache coding, check the qvalue and return.
      //@
      /*
      if (value == Http::Headers::get().AcceptEncodingValues.Cache) {
        const bool is_cache = !StringUtil::caseCompare(q_value, ZeroQvalueString);
        if (is_cache) {
          config_->stats().header_cache_.inc();
          return true;
        }
        config_->stats().header_not_valid_.inc();
        return false;
        }*/
      // If value is the identity coding, return false. The data should
      // not be transformed in this case.
      // https://www.w3.org/Protocols/rfc2616/rfc2616-sec3.html#sec3.5.
      if (value == Http::Headers::get().AcceptEncodingValues.Identity) {
        config_->stats().header_identity_.inc();
        return false;
      }
      // Otherwise, check if the header contains the wildcard. If so,
      // mark as true. Use this as the very last resort, as cache or
      // identity is weighted higher. Note that this filter disregards
      // order/priority at this time.
      if (value == Http::Headers::get().AcceptEncodingValues.Wildcard) {
        is_wildcard = !StringUtil::caseCompare(q_value, ZeroQvalueString);
      }
    }
    // If neither identity nor cache codings are present, return the result of the wildcard.
    if (is_wildcard) {
      config_->stats().header_wildcard_.inc();
      return true;
    }
    config_->stats().header_not_valid_.inc();
    return false;
  }
  config_->stats().no_accept_header_.inc();
  // If no accept-encoding header is present, return false.
  return false;
}

bool CacheFilter::isContentTypeAllowed(Http::HeaderMap& headers) const {
  const Http::HeaderEntry* content_type = headers.ContentType();
  if (content_type && !config_->contentTypeValues().empty()) {
    std::string value{StringUtil::trim(StringUtil::cropRight(content_type->value().c_str(), ";"))};
    return config_->contentTypeValues().find(value) != config_->contentTypeValues().end();
  }

  return true;
}

bool CacheFilter::isEtagAllowed(Http::HeaderMap& headers) const {
  const bool is_etag_allowed = !(config_->disableOnEtagHeader() && headers.Etag());
  if (!is_etag_allowed) {
    config_->stats().not_compressed_etag_.inc();
  }
  return is_etag_allowed;
}

bool CacheFilter::isMinimumContentLength(Http::HeaderMap& headers) const {
  const Http::HeaderEntry* content_length = headers.ContentLength();
  if (content_length) {
    uint64_t length;
    const bool is_minimum_content_length =
        StringUtil::atoul(content_length->value().c_str(), length) &&
        length >= config_->minimumLength();
    if (!is_minimum_content_length) {
      config_->stats().content_length_too_small_.inc();
    }
    return is_minimum_content_length;
  }

  const Http::HeaderEntry* transfer_encoding = headers.TransferEncoding();
  return (transfer_encoding &&
          StringUtil::caseFindToken(transfer_encoding->value().c_str(), ",",
                                    Http::Headers::get().TransferEncodingValues.Chunked.c_str()));
}

bool CacheFilter::isTransferEncodingAllowed(Http::HeaderMap& headers) const {
  const Http::HeaderEntry* transfer_encoding = headers.TransferEncoding();
  if (transfer_encoding) {
    for (auto header_value :
         // TODO(gsagula): add Http::HeaderMap::string_view() so string length doesn't need to be
         // computed twice. Find all other sites where this can be improved.
         StringUtil::splitToken(transfer_encoding->value().c_str(), ",", true)) {
      const auto trimmed_value = StringUtil::trim(header_value);
      if (//@StringUtil::caseCompare(trimmed_value,
              //@                        Http::Headers::get().TransferEncodingValues.Cache) ||
          StringUtil::caseCompare(trimmed_value,
                                  Http::Headers::get().TransferEncodingValues.Deflate)) {
        return false;
      }
    }
  }

  return true;
}

void CacheFilter::insertVaryHeader(Http::HeaderMap& headers) {
  const Http::HeaderEntry* vary = headers.Vary();
  if (vary) {
    if (!StringUtil::findToken(vary->value().c_str(), ",",
                               Http::Headers::get().VaryValues.AcceptEncoding, true)) {
      std::string new_header;
      absl::StrAppend(&new_header, vary->value().c_str(), ", ",
                      Http::Headers::get().VaryValues.AcceptEncoding);
      headers.insertVary().value(new_header);
    }
  } else {
    headers.insertVary().value(Http::Headers::get().VaryValues.AcceptEncoding);
  }
}

// TODO(gsagula): It seems that every proxy has a different opinion how to handle Etag. Some
// discussions around this topic have been going on for over a decade, e.g.,
// https://bz.apache.org/bugzilla/show_bug.cgi?id=45023
// This design attempts to stay more on the safe side by preserving weak etags and removing
// the strong ones when disable_on_etag_header is false. Envoy does NOT re-write entity tags.
void CacheFilter::sanitizeEtagHeader(Http::HeaderMap& headers) {
  const Http::HeaderEntry* etag = headers.Etag();
  if (etag) {
    absl::string_view value(etag->value().c_str());
    if (value.length() > 2 && !((value[0] == 'w' || value[0] == 'W') && value[1] == '/')) {
      headers.removeEtag();
    }
  }
}

} // namespace Cache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
