#include "common/cache/simple_cache.h"
#include "common/common/utility.h"

#include "absl/strings/str_join.h"
#include "gtest/gtest.h"

namespace Envoy {
namespace Cache {

class SimpleCacheTest : public testing::Test /*CacheTestBase*/ {
protected:
  SimpleCacheTest()
      : cache_((new SimpleCache)->self()), status_(DataStatus::Error),
        current_time_(time_source_.currentTime()) {}

  ~SimpleCacheTest() {
    BackendSharedPtr cache = cache_;
    cache_ = nullptr;
    if (cache->IsHealthy()) {
      cache->Shutdown(nullptr);
    }
  }

  // Writes a value into the cache.
  void checkPut(const std::string& key, const std::string& value) { checkPut(Cache(), key, value); }

  void checkPut(BackendSharedPtr cache, const std::string& key, const std::string& value) {
    DataReceiverFn inserter = cache->insert(makeKey(key));
    Value val = std::make_shared<ValueStruct>();
    val->timestamp_ = current_time_;
    val->value_ = value;
    inserter(DataStatus::LastChunk, val);
    PostOpCleanup();
  }

  void checkRemove(const std::string& key) {
    Cache()->remove(makeKey(key), nullptr);
    PostOpCleanup();
  }

  // Performs a Get and verifies that the key is not found.
  void checkNotFound(const char* key) { checkNotFound(Cache(), key); }

  void checkNotFound(BackendSharedPtr cache, absl::string_view key) {
    InitiateGet(cache, key);
    EXPECT_EQ(DataStatus::NotFound, status_);
  }

  // Initiate a cache Get, and return the Callback* which can be
  // passed to WaitAndCheck or WaitAndcheckNotFound.
  void initiateGet(absl::string_view key) { return initiateGet(Cache(), key); }

  void initiateGet(BackendSharedPtr cache, absl::string_view key) {
    /*
    {
      ScopedMutex lock(mutex_.get());
      ++outstanding_fetches_;
    }
    */
    value_ = std::make_shared<ValueStruct>();
    LookupContextPtr lookup = cache->lookup(makeKey(key));
    nextChunk(std::move(lookup));
  }

  void nextChunk(LookupContextPtr lookup) {
    lookup->read([this, &lookup](DataStatus status, const Value& value) {
      if (ValidStatus(status)) {
        absl::StrAppend(&value_->value_, value->value_);
      }
      if (TerminalStatus(status)) {
        status_ = status;
      } else {
        nextChunk(std::move(lookup));
      }
      return ReceiverStatus::Ok;
    });
  }

  // Performs a cache Get, waits for callback completion, and checks the
  // result is as expected.
  void checkGet(absl::string_view key, absl::string_view expected_value) {
    checkGet(Cache(), key, expected_value);
  }

  void checkGet(BackendSharedPtr cache, absl::string_view key, absl::string_view expected_value) {
    initiateGet(cache, key);
    EXPECT_EQ(expected_value, value_->value_);
  }

  BackendSharedPtr Cache() { return cache_; }
  void PostOpCleanup() { /*cache_->SanityCheck();*/
  }

  BackendSharedPtr cache_;
  Value value_;
  DataStatus status_;
  ProdMonotonicTimeSource time_source_;
  MonotonicTime current_time_;
};

// Simple flow of putting in an item, getting it, deleting it.
TEST_F(SimpleCacheTest, PutGetRemove) {
  // EXPECT_EQ(static_cast<size_t>(0), cache_->size_bytes());
  // EXPECT_EQ(static_cast<size_t>(0), cache_->num_elements());
  checkPut("Name", "Value");
  checkGet("Name", "Value");
  // EXPECT_EQ(static_cast<size_t>(9), cache_->size_bytes());  // "Name" + "Value"
  // EXPECT_EQ(static_cast<size_t>(1), cache_->num_elements());
  checkNotFound("Another Name");

  checkPut("Name", "NewValue");
  checkGet("Name", "NewValue");
  // EXPECT_EQ(static_cast<size_t>(12),
  //          cache_->size_bytes());  // "Name" + "NewValue"
  // EXPECT_EQ(static_cast<size_t>(1), cache_->num_elements());

  cache_->remove(makeKey("Name"), nullptr);
  // cache_->SanityCheck();
  Value value_buffer;
  checkNotFound("Name");
  // EXPECT_EQ(static_cast<size_t>(0), cache_->size_bytes());
  // EXPECT_EQ(static_cast<size_t>(0), cache_->num_elements());
}

/*TEST_F(SimpleCacheTest, RemoveWithPrefix) {
  checkPut("N1", "Value1");
  checkPut("N2", "Value2");
  checkPut("M3", "Value3");
  checkPut("M4", "Value4");

  // 4*(strlen("N1") + strlen("Value1")) = 4*(2 + 6) = 32
  //EXPECT_EQ(static_cast<size_t>(32), cache_->size_bytes());
  //EXPECT_EQ(static_cast<size_t>(4), cache_->num_elements());

  cache_->removeWithPrefixForTesting("N");
  EXPECT_EQ(static_cast<size_t>(16), cache_->size_bytes());
  EXPECT_EQ(static_cast<size_t>(2), cache_->num_elements());
  checkNotFound("N1");
  checkNotFound("N2");
  checkGet("M3", "Value3");
  checkGet("M4", "Value4");

  cache_->removeWithPrefixForTesting("M");
  EXPECT_EQ(static_cast<size_t>(0), cache_->size_bytes());
  EXPECT_EQ(static_cast<size_t>(0), cache_->num_elements());
  checkNotFound("N1");
  checkNotFound("N2");
  checkNotFound("M3");
  checkNotFound("M4");
  }*/

// Test eviction. We happen to know that the cache does not account for
// STL overhead -- it's just counting key/value size. Exploit that to
// understand when objects fall off the end.
/*TEST_F(SimpleCacheTest, LeastRecentlyUsed) {
  // Fill the cache.
  std::string keys[10], values[10];
  const char key_pattern[]      = "name%d";
  const char value_pattern[]    = "valu%d";
  const int key_plus_value_size = 10;  // strlen("name7") + strlen("valu7")
  const size_t num_elements        = kMaxSize / key_plus_value_size;
  for (int i = 0; i < 10; ++i) {
    SStringPrintf(&keys[i], key_pattern, i);
    SStringPrintf(&values[i], value_pattern, i);
    checkPut(keys[i], values[i]);
  }
  EXPECT_EQ(kMaxSize, cache_->size_bytes());
  EXPECT_EQ(num_elements, cache_->num_elements());

  // Ensure we can see those.
  for (int i = 0; i < 10; ++i) {
    checkGet(keys[i], values[i]);
  }

  // Now if we insert a new entry totaling 10 bytes, that should work,
  // but we will lose name0 due to LRU semantics. We should still have name1,
  // and by Get-ing name1 it we will make it the MRU.
  checkPut("nameA", "valuA");
  checkGet("nameA", "valuA");
  checkNotFound("name0");
  checkGet("name1", "valu1");

  // So now when we put in nameB,valuB we will lose name2 but keep name1,
  // which got bumped up to the MRU when we checked it above.
  checkPut("nameB", "valuB");
  checkGet("nameB", "valuB");
  checkGet("name1", "valu1");
  checkNotFound("name2");

  // Now insert something 1 byte too big, spelling out "value" this time.
  // We will now lose name3 and name4. We should still have name5-name9,
  // plus name1, nameA, and nameB.
  checkPut("nameC", "valueC");
  checkNotFound("name3");
  checkNotFound("name4");
  checkGet("nameA", "valuA");
  checkGet("nameB", "valuB");
  checkGet("nameC", "valueC");
  checkGet("name1", "valu1");
  for (int i = 5; i < 10; ++i) {
    checkGet(keys[i], values[i]);
  }

  // Now the oldest item is "nameA". Freshen it by re-inserting it, tickling
  // the code-path in lru_cache.cc that special-cases handling of re-inserting
  // the same value.
  checkPut("nameA", "valuA");
  checkPut("nameD", "valuD");
  // nameB should be evicted, the others should be retained.
  checkNotFound("nameB");
  checkGet("nameA", "valuA");
  checkGet("nameC", "valueC");
  checkGet("name1", "valu1");
  for (int i = 5; i < 10; ++i) {
    checkGet(keys[i], values[i]);
  }
  }*/

/*
TEST_F(SimpleCacheTest, BasicInvalid) {
  // Check that we honor callback veto on validity.
  checkPut("nameA", "valueA");
  checkPut("nameB", "valueB");
  checkGet("nameA", "valueA");
  checkGet("nameB", "valueB");
  set_invalid_value("valueA");
  checkNotFound("nameA");
  checkGet("nameB", "valueB");
  }*/

/*TEST_F(SimpleCacheTest, MultiGet) {
  // This covers CacheInterface's default implementation of MultiGet.
  TestMultiGet();
  }*/

TEST_F(SimpleCacheTest, KeyNotFoundWhenUnhealthy) {
  checkPut("nameA", "valueA");
  cache_->Shutdown(nullptr);
  checkNotFound("nameA");
}

/*
// Cache starts in 'healthy' state and it should be healthy before
// performing any checks, otherwise Get will return 'not found'
TEST_F(SimpleCacheTest, DoesNotPutWhenUnhealthy) {
  checkPut("nameA", "valueA");

  cache_->Shutdown(nullptr);
  checkNotFound("nameA");
}

TEST_F(SimpleCacheTest, DoesNotDeleteWhenUnhealthy) {
  checkPut("nameA", "valueA");
  cache_->set_is_healthy(false);
  CheckDelete("nameA");

  cache_->set_is_healthy(true);
  checkGet("nameA", "valueA");
  }

TEST_F(SimpleCacheTest, DoesNotDeleteWithPrefixWhenUnhealthy) {
  checkPut("nameA", "valueA");
  cache_->set_is_healthy(false);
  cache_->removeWithPrefixForTesting("name");

  cache_->set_is_healthy(true);
  checkGet("nameA", "valueA");
  }*/

} // namespace Cache
} // namespace Envoy
