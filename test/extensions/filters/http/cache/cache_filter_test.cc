#include "common/compressor/zlib_compressor_impl.h"
#include "common/decompressor/zlib_decompressor_impl.h"
#include "common/protobuf/utility.h"

#include "extensions/filters/http/cache/cache_filter.h"

#include "test/mocks/http/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/stats/mocks.h"
#include "test/test_common/utility.h"

#include "gtest/gtest.h"

using testing::Return;

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Cache {

class CacheFilterTest : public testing::Test {
protected:
  CacheFilterTest() {
    ON_CALL(runtime_.snapshot_, featureEnabled("cache.filter_enabled", 100))
        .WillByDefault(Return(true));
  }

  void SetUp() override {
    setUpFilter("{}");
    decompressor_.init(31);
  }

  // CacheFilter private member functions
  void sanitizeEtagHeader(Http::HeaderMap& headers) { filter_->sanitizeEtagHeader(headers); }

  void insertVaryHeader(Http::HeaderMap& headers) { filter_->insertVaryHeader(headers); }

  bool isContentTypeAllowed(Http::HeaderMap& headers) {
    return filter_->isContentTypeAllowed(headers);
  }

  bool isEtagAllowed(Http::HeaderMap& headers) { return filter_->isEtagAllowed(headers); }

  bool hasCacheControlNoTransform(Http::HeaderMap& headers) {
    return filter_->hasCacheControlNoTransform(headers);
  }

  bool isAcceptEncodingAllowed(Http::HeaderMap& headers) {
    return filter_->isAcceptEncodingAllowed(headers);
  }

  bool isMinimumContentLength(Http::HeaderMap& headers) {
    return filter_->isMinimumContentLength(headers);
  }

  bool isTransferEncodingAllowed(Http::HeaderMap& headers) {
    return filter_->isTransferEncodingAllowed(headers);
  }

  // CacheFilterTest Helpers
  void setUpFilter(std::string&& json) {
    Json::ObjectSharedPtr config = Json::Factory::loadFromString(json);
    envoy::config::filter::http::cache::v2::Cache cache;
    MessageUtil::loadFromJson(json, cache);
    config_.reset(new CacheFilterConfig(cache, "test.", stats_, runtime_));
    filter_.reset(new CacheFilter(config_));
  }

  void verifyCompressedData() {
    decompressor_.decompress(data_, decompressed_data_);
    const std::string uncompressed_str{decompressed_data_.toString()};
    ASSERT_EQ(expected_str_.length(), uncompressed_str.length());
    EXPECT_EQ(expected_str_, uncompressed_str);
    EXPECT_EQ(expected_str_.length(),
              stats_.counter("test.cache.total_uncompressed_bytes").value());
    EXPECT_EQ(data_.length(), stats_.counter("test.cache.total_compressed_bytes").value());
  }

  void feedBuffer(uint64_t size) {
    TestUtility::feedBufferWithRandomCharacters(data_, size);
    expected_str_ += data_.toString();
  }

  void drainBuffer() {
    const uint64_t data_len = data_.length();
    data_.drain(data_len);
  }

  void doRequest(Http::TestHeaderMapImpl&& headers, bool end_stream) {
    EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, end_stream));
  }

  void doResponseCompression(Http::TestHeaderMapImpl&& headers) {
    uint64_t content_length;
    ASSERT_TRUE(StringUtil::atoul(headers.get_("content-length").c_str(), content_length));
    feedBuffer(content_length);
    EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(headers, false));
    EXPECT_EQ("", headers.get_("content-length"));
    EXPECT_EQ(Http::Headers::get().ContentEncodingValues.Cache, headers.get_("content-encoding"));
    EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(data_, true));
    verifyCompressedData();
    drainBuffer();
    EXPECT_EQ(1U, stats_.counter("test.cache.compressed").value());
  }

  void doResponseNoCompression(Http::TestHeaderMapImpl&& headers) {
    uint64_t content_length;
    ASSERT_TRUE(StringUtil::atoul(headers.get_("content-length").c_str(), content_length));
    feedBuffer(content_length);
    Http::TestHeaderMapImpl continue_headers;
    EXPECT_EQ(Http::FilterHeadersStatus::Continue,
              filter_->encode100ContinueHeaders(continue_headers));
    EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(headers, false));
    EXPECT_EQ("", headers.get_("content-encoding"));
    EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(data_, false));
    Http::TestHeaderMapImpl trailers;
    EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(trailers));
    EXPECT_EQ(1, stats_.counter("test.cache.not_compressed").value());
  }

  CacheFilterConfigSharedPtr config_;
  std::unique_ptr<CacheFilter> filter_;
  Buffer::OwnedImpl data_;
  Decompressor::ZlibDecompressorImpl decompressor_;
  Buffer::OwnedImpl decompressed_data_;
  std::string expected_str_;
  Stats::IsolatedStoreImpl stats_;
  NiceMock<Runtime::MockLoader> runtime_;
};

// Test if Runtime Feature is Disabled
TEST_F(CacheFilterTest, RuntimeDisabled) {
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("cache.filter_enabled", 100))
      .WillOnce(Return(false));
  doRequest({{":method", "get"}, {"accept-encoding", "deflate, cache"}}, false);
  doResponseNoCompression({{":method", "get"}, {"content-length", "256"}});
}

// Default config values.
TEST_F(CacheFilterTest, DefaultConfigValues) {
  EXPECT_EQ(5, config_->memoryLevel());
  EXPECT_EQ(30, config_->minimumLength());
  EXPECT_EQ(28, config_->windowBits());
  EXPECT_EQ(false, config_->disableOnEtagHeader());
  EXPECT_EQ(false, config_->removeAcceptEncodingHeader());
  EXPECT_EQ(Compressor::ZlibCompressorImpl::CompressionStrategy::Standard,
            config_->compressionStrategy());
  EXPECT_EQ(Compressor::ZlibCompressorImpl::CompressionLevel::Standard,
            config_->compressionLevel());
  EXPECT_EQ(8, config_->contentTypeValues().size());
}

// Acceptance Testing with default configuration.
TEST_F(CacheFilterTest, AcceptanceCacheEncoding) {
  doRequest({{":method", "get"}, {"accept-encoding", "deflate, cache"}}, false);
  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data, false));
  Http::TestHeaderMapImpl trailers;
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(trailers));
  doResponseCompression({{":method", "get"}, {"content-length", "256"}});
}

// Verifies isAcceptEncodingAllowed function.
TEST_F(CacheFilterTest, hasCacheControlNoTransform) {
  {
    Http::TestHeaderMapImpl headers = {{"cache-control", "no-cache"}};
    EXPECT_FALSE(hasCacheControlNoTransform(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {{"cache-control", "no-transform"}};
    EXPECT_TRUE(hasCacheControlNoTransform(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {{"cache-control", "No-Transform"}};
    EXPECT_TRUE(hasCacheControlNoTransform(headers));
  }
}

// Verifies that compression is skipped when cache-control header has no-tranform value.
TEST_F(CacheFilterTest, hasCacheControlNoTransformNoCompression) {
  doRequest({{":method", "get"}, {"accept-encoding", "cache;q=0, deflate"}}, true);
  doResponseNoCompression(
      {{":method", "get"}, {"content-length", "256"}, {"cache-control", "no-transform"}});
}

// Verifies that compression is NOT skipped when cache-control header does NOT have no-tranform
// value.
TEST_F(CacheFilterTest, hasCacheControlNoTransformCompression) {
  doRequest({{":method", "get"}, {"accept-encoding", "cache, deflate"}}, true);
  doResponseCompression(
      {{":method", "get"}, {"content-length", "256"}, {"cache-control", "no-cache"}});
}

// Verifies isAcceptEncodingAllowed function.
TEST_F(CacheFilterTest, isAcceptEncodingAllowed) {
  {
    Http::TestHeaderMapImpl headers = {{"accept-encoding", "deflate, cache, br"}};
    EXPECT_TRUE(isAcceptEncodingAllowed(headers));
    EXPECT_EQ(1, stats_.counter("test.cache.header_cache").value());
  }
  {
    Http::TestHeaderMapImpl headers = {{"accept-encoding", "deflate, cache;q=1.0, *;q=0.5"}};
    EXPECT_TRUE(isAcceptEncodingAllowed(headers));
    EXPECT_EQ(2, stats_.counter("test.cache.header_cache").value());
  }
  {
    Http::TestHeaderMapImpl headers = {
        {"accept-encoding", "\tdeflate\t, cache\t ; q\t =\t 1.0,\t * ;q=0.5\n"}};
    EXPECT_TRUE(isAcceptEncodingAllowed(headers));
    EXPECT_EQ(3, stats_.counter("test.cache.header_cache").value());
  }
  {
    Http::TestHeaderMapImpl headers = {{"accept-encoding", "deflate,cache;q=1.0,*;q=0"}};
    EXPECT_TRUE(isAcceptEncodingAllowed(headers));
    EXPECT_EQ(4, stats_.counter("test.cache.header_cache").value());
  }
  {
    Http::TestHeaderMapImpl headers = {{"accept-encoding", "deflate, cache;q=0.2, br;q=1"}};
    EXPECT_TRUE(isAcceptEncodingAllowed(headers));
    EXPECT_EQ(5, stats_.counter("test.cache.header_cache").value());
  }
  {
    Http::TestHeaderMapImpl headers = {{"accept-encoding", "*"}};
    EXPECT_TRUE(isAcceptEncodingAllowed(headers));
    EXPECT_EQ(1, stats_.counter("test.cache.header_wildcard").value());
  }
  {
    Http::TestHeaderMapImpl headers = {{"accept-encoding", "*;q=1"}};
    EXPECT_TRUE(isAcceptEncodingAllowed(headers));
    EXPECT_EQ(2, stats_.counter("test.cache.header_wildcard").value());
  }
  {
    // cache header is not valid due to q=0.
    Http::TestHeaderMapImpl headers = {{"accept-encoding", "cache;q=0,*;q=1"}};
    EXPECT_FALSE(isAcceptEncodingAllowed(headers));
    EXPECT_EQ(5, stats_.counter("test.cache.header_cache").value());
    EXPECT_EQ(1, stats_.counter("test.cache.header_not_valid").value());
  }
  {
    Http::TestHeaderMapImpl headers = {};
    EXPECT_FALSE(isAcceptEncodingAllowed(headers));
    EXPECT_EQ(1, stats_.counter("test.cache.no_accept_header").value());
  }
  {
    Http::TestHeaderMapImpl headers = {{"accept-encoding", "identity, *;q=0"}};
    EXPECT_FALSE(isAcceptEncodingAllowed(headers));
    EXPECT_EQ(1, stats_.counter("test.cache.header_identity").value());
  }
  {
    Http::TestHeaderMapImpl headers = {{"accept-encoding", "identity;q=0.5, *;q=0"}};
    EXPECT_FALSE(isAcceptEncodingAllowed(headers));
    EXPECT_EQ(2, stats_.counter("test.cache.header_identity").value());
  }
  {
    Http::TestHeaderMapImpl headers = {{"accept-encoding", "identity;q=0, *;q=0"}};
    EXPECT_FALSE(isAcceptEncodingAllowed(headers));
    EXPECT_EQ(3, stats_.counter("test.cache.header_identity").value());
  }
  {
    Http::TestHeaderMapImpl headers = {{"accept-encoding", "xyz;q=1, br;q=0.2, *"}};
    EXPECT_TRUE(isAcceptEncodingAllowed(headers));
    EXPECT_EQ(3, stats_.counter("test.cache.header_wildcard").value());
  }
  {
    Http::TestHeaderMapImpl headers = {{"accept-encoding", "xyz;q=1, br;q=0.2, *;q=0"}};
    EXPECT_FALSE(isAcceptEncodingAllowed(headers));
    EXPECT_EQ(3, stats_.counter("test.cache.header_wildcard").value());
    EXPECT_EQ(2, stats_.counter("test.cache.header_not_valid").value());
  }
  {
    Http::TestHeaderMapImpl headers = {{"accept-encoding", "xyz;q=1, br;q=0.2"}};
    EXPECT_FALSE(isAcceptEncodingAllowed(headers));
    EXPECT_EQ(3, stats_.counter("test.cache.header_not_valid").value());
  }
  {
    Http::TestHeaderMapImpl headers = {{"accept-encoding", "identity"}};
    EXPECT_FALSE(isAcceptEncodingAllowed(headers));
    EXPECT_EQ(4, stats_.counter("test.cache.header_identity").value());
  }
  {
    Http::TestHeaderMapImpl headers = {{"accept-encoding", "identity;q=1"}};
    EXPECT_FALSE(isAcceptEncodingAllowed(headers));
    EXPECT_EQ(5, stats_.counter("test.cache.header_identity").value());
  }
  {
    Http::TestHeaderMapImpl headers = {{"accept-encoding", "identity;q=0"}};
    EXPECT_FALSE(isAcceptEncodingAllowed(headers));
    EXPECT_EQ(6, stats_.counter("test.cache.header_identity").value());
  }
  {
    // Test that we return identity and ignore the invalid wildcard.
    Http::TestHeaderMapImpl headers = {{"accept-encoding", "identity, *;q=0"}};
    EXPECT_FALSE(isAcceptEncodingAllowed(headers));
    EXPECT_EQ(7, stats_.counter("test.cache.header_identity").value());
    EXPECT_EQ(3, stats_.counter("test.cache.header_not_valid").value());
  }
  {
    Http::TestHeaderMapImpl headers = {{"accept-encoding", "deflate, cache;Q=.5, br"}};
    EXPECT_TRUE(isAcceptEncodingAllowed(headers));
    EXPECT_EQ(6, stats_.counter("test.cache.header_cache").value());
  }
  {
    Http::TestHeaderMapImpl headers = {{"accept-encoding", "identity;Q=0"}};
    EXPECT_FALSE(isAcceptEncodingAllowed(headers));
    EXPECT_EQ(8, stats_.counter("test.cache.header_identity").value());
  }
}

// Verifies that compression is skipped when accept-encoding header is not allowed.
TEST_F(CacheFilterTest, AcceptEncodingNoCompression) {
  doRequest({{":method", "get"}, {"accept-encoding", "cache;q=0, deflate"}}, true);
  doResponseNoCompression({{":method", "get"}, {"content-length", "256"}});
}

// Verifies that compression is NOT skipped when accept-encoding header is allowed.
TEST_F(CacheFilterTest, AcceptEncodingCompression) {
  doRequest({{":method", "get"}, {"accept-encoding", "cache, deflate"}}, true);
  doResponseCompression({{":method", "get"}, {"content-length", "256"}});
}

// Verifies isMinimumContentLength function.
TEST_F(CacheFilterTest, isMinimumContentLength) {
  {
    Http::TestHeaderMapImpl headers = {{"content-length", "31"}};
    EXPECT_TRUE(isMinimumContentLength(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {{"content-length", "29"}};
    EXPECT_FALSE(isMinimumContentLength(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {{"transfer-encoding", "chunked"}};
    EXPECT_TRUE(isMinimumContentLength(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {{"transfer-encoding", "Chunked"}};
    EXPECT_TRUE(isMinimumContentLength(headers));
  }

  setUpFilter(R"EOF({"content_length": 500})EOF");
  {
    Http::TestHeaderMapImpl headers = {{"content-length", "501"}};
    EXPECT_TRUE(isMinimumContentLength(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {{"transfer-encoding", "chunked"}};
    EXPECT_TRUE(isMinimumContentLength(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {{"content-length", "499"}};
    EXPECT_FALSE(isMinimumContentLength(headers));
  }
}

// Verifies that compression is skipped when content-length header is NOT allowed.
TEST_F(CacheFilterTest, ContentLengthNoCompression) {
  doRequest({{":method", "get"}, {"accept-encoding", "cache"}}, true);
  doResponseNoCompression({{":method", "get"}, {"content-length", "10"}});
}

// Verifies that compression is NOT skipped when content-length header is allowed.
TEST_F(CacheFilterTest, ContentLengthCompression) {
  setUpFilter(R"EOF({"content_length": 500})EOF");
  doRequest({{":method", "get"}, {"accept-encoding", "cache"}}, true);
  doResponseCompression({{":method", "get"}, {"content-length", "1000"}});
}

// Verifies isContentTypeAllowed function.
TEST_F(CacheFilterTest, isContentTypeAllowed) {

  {
    Http::TestHeaderMapImpl headers = {{"content-type", "text/html"}};
    EXPECT_TRUE(isContentTypeAllowed(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {{"content-type", "text/xml"}};
    EXPECT_TRUE(isContentTypeAllowed(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {{"content-type", "text/plain"}};
    EXPECT_TRUE(isContentTypeAllowed(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {{"content-type", "application/javascript"}};
    EXPECT_TRUE(isContentTypeAllowed(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {{"content-type", "image/svg+xml"}};
    EXPECT_TRUE(isContentTypeAllowed(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {{"content-type", "application/json;charset=utf-8"}};
    EXPECT_TRUE(isContentTypeAllowed(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {{"content-type", "application/json"}};
    EXPECT_TRUE(isContentTypeAllowed(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {{"content-type", "application/xhtml+xml"}};
    EXPECT_TRUE(isContentTypeAllowed(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {{"content-type", "Application/XHTML+XML"}};
    EXPECT_TRUE(isContentTypeAllowed(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {{"content-type", "image/jpeg"}};
    EXPECT_FALSE(isContentTypeAllowed(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {};
    EXPECT_TRUE(isContentTypeAllowed(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {{"content-type", "\ttext/html\t\n"}};
    EXPECT_TRUE(isContentTypeAllowed(headers));
  }

  setUpFilter(R"EOF(
    {
      "content_type": [
        "text/html",
        "xyz/svg+xml",
        "Test/INSENSITIVE"
      ]
    }
  )EOF");

  {
    Http::TestHeaderMapImpl headers = {{"content-type", "xyz/svg+xml"}};
    EXPECT_TRUE(isContentTypeAllowed(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {};
    EXPECT_TRUE(isContentTypeAllowed(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {{"content-type", "xyz/false"}};
    EXPECT_FALSE(isContentTypeAllowed(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {{"content-type", "image/jpeg"}};
    EXPECT_FALSE(isContentTypeAllowed(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {{"content-type", "test/insensitive"}};
    EXPECT_TRUE(isContentTypeAllowed(headers));
  }
}

// Verifies that compression is skipped when content-encoding header is NOT allowed.
TEST_F(CacheFilterTest, ContentTypeNoCompression) {
  setUpFilter(R"EOF(
    {
      "content_type": [
        "text/html",
        "text/css",
        "text/plain",
        "application/javascript",
        "application/json",
        "font/eot",
        "image/svg+xml"
      ]
    }
  )EOF");
  doRequest({{":method", "get"}, {"accept-encoding", "cache"}}, true);
  doResponseNoCompression(
      {{":method", "get"}, {"content-length", "256"}, {"content-type", "image/jpeg"}});
}

// Verifies that compression is NOT skipped when content-encoding header is allowed.
TEST_F(CacheFilterTest, ContentTypeCompression) {
  doRequest({{":method", "get"}, {"accept-encoding", "cache"}}, true);
  doResponseCompression({{":method", "get"},
                         {"content-length", "256"},
                         {"content-type", "application/json;charset=utf-8"}});
}

// Verifies sanitizeEtagHeader function.
TEST_F(CacheFilterTest, sanitizeEtagHeader) {
  {
    std::string etag_header{R"EOF(W/"686897696a7c876b7e")EOF"};
    Http::TestHeaderMapImpl headers = {{"etag", etag_header}};
    sanitizeEtagHeader(headers);
    EXPECT_EQ(etag_header, headers.get_("etag"));
  }
  {
    std::string etag_header{R"EOF(w/"686897696a7c876b7e")EOF"};
    Http::TestHeaderMapImpl headers = {{"etag", etag_header}};
    sanitizeEtagHeader(headers);
    EXPECT_EQ(etag_header, headers.get_("etag"));
  }
  {
    Http::TestHeaderMapImpl headers = {{"etag", "686897696a7c876b7e"}};
    sanitizeEtagHeader(headers);
    EXPECT_FALSE(headers.has("etag"));
  }
}

// Verifies isEtagAllowed function.
TEST_F(CacheFilterTest, isEtagAllowed) {
  {
    Http::TestHeaderMapImpl headers = {{"etag", R"EOF(W/"686897696a7c876b7e")EOF"}};
    EXPECT_TRUE(isEtagAllowed(headers));
    EXPECT_EQ(0, stats_.counter("test.cache.not_compressed_etag").value());
  }
  {
    Http::TestHeaderMapImpl headers = {{"etag", "686897696a7c876b7e"}};
    EXPECT_TRUE(isEtagAllowed(headers));
    EXPECT_EQ(0, stats_.counter("test.cache.not_compressed_etag").value());
  }
  {
    Http::TestHeaderMapImpl headers = {};
    EXPECT_TRUE(isEtagAllowed(headers));
    EXPECT_EQ(0, stats_.counter("test.cache.not_compressed_etag").value());
  }

  setUpFilter(R"EOF({ "disable_on_etag_header": true })EOF");
  {
    Http::TestHeaderMapImpl headers = {{"etag", R"EOF(W/"686897696a7c876b7e")EOF"}};
    EXPECT_FALSE(isEtagAllowed(headers));
    EXPECT_EQ(1, stats_.counter("test.cache.not_compressed_etag").value());
  }
  {
    Http::TestHeaderMapImpl headers = {{"etag", "686897696a7c876b7e"}};
    EXPECT_FALSE(isEtagAllowed(headers));
    EXPECT_EQ(2, stats_.counter("test.cache.not_compressed_etag").value());
  }
  {
    Http::TestHeaderMapImpl headers = {};
    EXPECT_TRUE(isEtagAllowed(headers));
    EXPECT_EQ(2, stats_.counter("test.cache.not_compressed_etag").value());
  }
}

// Verifies that compression is skipped when etag header is NOT allowed.
TEST_F(CacheFilterTest, EtagNoCompression) {
  setUpFilter(R"EOF({ "disable_on_etag_header": true })EOF");
  doRequest({{":method", "get"}, {"accept-encoding", "cache"}}, true);
  doResponseNoCompression(
      {{":method", "get"}, {"content-length", "256"}, {"etag", R"EOF(W/"686897696a7c876b7e")EOF"}});
  EXPECT_EQ(1, stats_.counter("test.cache.not_compressed_etag").value());
}

// Verifies that compression is skipped when etag header is NOT allowed.
TEST_F(CacheFilterTest, EtagCompression) {
  doRequest({{":method", "get"}, {"accept-encoding", "cache"}}, true);
  Http::TestHeaderMapImpl headers{
      {":method", "get"}, {"content-length", "256"}, {"etag", "686897696a7c876b7e"}};
  feedBuffer(256);
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(headers, false));
  EXPECT_FALSE(headers.has("etag"));
  EXPECT_EQ("cache", headers.get_("content-encoding"));
}

// Verifies isTransferEncodingAllowed function.
TEST_F(CacheFilterTest, isTransferEncodingAllowed) {
  {
    Http::TestHeaderMapImpl headers = {};
    EXPECT_TRUE(isTransferEncodingAllowed(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {{"transfer-encoding", "chunked"}};
    EXPECT_TRUE(isTransferEncodingAllowed(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {{"transfer-encoding", "Chunked"}};
    EXPECT_TRUE(isTransferEncodingAllowed(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {{"transfer-encoding", "deflate"}};
    EXPECT_FALSE(isTransferEncodingAllowed(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {{"transfer-encoding", "Deflate"}};
    EXPECT_FALSE(isTransferEncodingAllowed(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {{"transfer-encoding", "cache"}};
    EXPECT_FALSE(isTransferEncodingAllowed(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {{"transfer-encoding", "cache, chunked"}};
    EXPECT_FALSE(isTransferEncodingAllowed(headers));
  }
  {
    Http::TestHeaderMapImpl headers = {{"transfer-encoding", " cache\t,  chunked\t\n"}};
    EXPECT_FALSE(isTransferEncodingAllowed(headers));
  }
}

// Tests compression when Transfer-Encoding header exists.
TEST_F(CacheFilterTest, TransferEncodingChunked) {
  doRequest({{":method", "get"}, {"accept-encoding", "cache"}}, true);
  doResponseCompression(
      {{":method", "get"}, {"content-length", "256"}, {"transfer-encoding", "chunked"}});
}

// Tests compression when Transfer-Encoding header exists.
TEST_F(CacheFilterTest, AcceptanceTransferEncodingCache) {

  doRequest({{":method", "get"}, {"accept-encoding", "cache"}}, true);
  doResponseNoCompression(
      {{":method", "get"}, {"content-length", "256"}, {"transfer-encoding", "chunked, deflate"}});
}

// Content-Encoding: upstream response is already encoded.
TEST_F(CacheFilterTest, ContentEncodingAlreadyEncoded) {
  doRequest({{":method", "get"}, {"accept-encoding", "cache"}}, true);
  Http::TestHeaderMapImpl response_headers{
      {":method", "get"}, {"content-length", "256"}, {"content-encoding", "deflate, cache"}};
  feedBuffer(256);
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, false));
  EXPECT_TRUE(response_headers.has("content-length"));
  EXPECT_FALSE(response_headers.has("transfer-encoding"));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(data_, false));
}

// No compression when upstream response is empty.
TEST_F(CacheFilterTest, EmptyResponse) {

  Http::TestHeaderMapImpl headers{{":method", "get"}, {":status", "204"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(headers, true));
  EXPECT_EQ("", headers.get_("content-length"));
  EXPECT_EQ("", headers.get_("content-encoding"));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(data_, true));
}

// Verifies insertVaryHeader function.
TEST_F(CacheFilterTest, insertVaryHeader) {
  {
    Http::TestHeaderMapImpl headers = {};
    insertVaryHeader(headers);
    EXPECT_EQ("Accept-Encoding", headers.get_("vary"));
  }
  {
    Http::TestHeaderMapImpl headers = {{"vary", "Cookie"}};
    insertVaryHeader(headers);
    EXPECT_EQ("Cookie, Accept-Encoding", headers.get_("vary"));
  }
  {
    Http::TestHeaderMapImpl headers = {{"vary", "accept-encoding"}};
    insertVaryHeader(headers);
    EXPECT_EQ("accept-encoding, Accept-Encoding", headers.get_("vary"));
  }
  {
    Http::TestHeaderMapImpl headers = {{"vary", "Accept-Encoding, Cookie"}};
    insertVaryHeader(headers);
    EXPECT_EQ("Accept-Encoding, Cookie", headers.get_("vary"));
  }
  {
    Http::TestHeaderMapImpl headers = {{"vary", "Accept-Encoding"}};
    insertVaryHeader(headers);
    EXPECT_EQ("Accept-Encoding", headers.get_("vary"));
  }
}

// Filter should set Vary header value with `accept-encoding`.
TEST_F(CacheFilterTest, NoVaryHeader) {
  doRequest({{":method", "get"}, {"accept-encoding", "cache"}}, true);
  Http::TestHeaderMapImpl headers{{":method", "get"}, {"content-length", "256"}};
  feedBuffer(256);
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(headers, false));
  EXPECT_TRUE(headers.has("vary"));
  EXPECT_EQ("Accept-Encoding", headers.get_("vary"));
}

// Filter should set Vary header value with `accept-encoding` and preserve other values.
TEST_F(CacheFilterTest, VaryOtherValues) {
  doRequest({{":method", "get"}, {"accept-encoding", "cache"}}, true);
  Http::TestHeaderMapImpl headers{
      {":method", "get"}, {"content-length", "256"}, {"vary", "User-Agent, Cookie"}};
  feedBuffer(256);
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(headers, false));
  EXPECT_TRUE(headers.has("vary"));
  EXPECT_EQ("User-Agent, Cookie, Accept-Encoding", headers.get_("vary"));
}

// Vary header should have only one `accept-encoding`value.
TEST_F(CacheFilterTest, VaryAlreadyHasAcceptEncoding) {
  doRequest({{":method", "get"}, {"accept-encoding", "cache"}}, true);
  Http::TestHeaderMapImpl headers{
      {":method", "get"}, {"content-length", "256"}, {"vary", "accept-encoding"}};
  feedBuffer(256);
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(headers, false));
  EXPECT_TRUE(headers.has("vary"));
  EXPECT_EQ("accept-encoding, Accept-Encoding", headers.get_("vary"));
}

// Verify removeAcceptEncoding header.
TEST_F(CacheFilterTest, RemoveAcceptEncodingHeader) {
  {
    Http::TestHeaderMapImpl headers = {{"accept-encoding", "deflate, cache, br"}};
    setUpFilter(R"EOF({"remove_accept_encoding_header": true})EOF");
    EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, true));
    EXPECT_FALSE(headers.has("accept-encoding"));
  }
  {
    Http::TestHeaderMapImpl headers = {{"accept-encoding", "deflate, cache, br"}};
    setUpFilter("{}");
    EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, true));
    EXPECT_TRUE(headers.has("accept-encoding"));
    EXPECT_EQ("deflate, cache, br", headers.get_("accept-encoding"));
  }
}

} // namespace Cache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
