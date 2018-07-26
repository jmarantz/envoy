#include "common/cache/simple_cache.h"
#include "common/common/utility.h"

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
  void CheckPut(const std::string& key, const std::string& value) { CheckPut(Cache(), key, value); }

  void CheckPut(BackendSharedPtr cache, const std::string& key, const std::string& value) {
    DataReceiverFn inserter = cache->insert(makeKey(key));
    Value val = std::make_shared<ValueStruct>();
    val->timestamp_ = current_time_;
    val->value_ = value;
    inserter(DataStatus::LastChunk, val);
    PostOpCleanup();
  }

  void CheckRemove(const std::string& key) {
    Cache()->remove(makeKey(key), nullptr);
    PostOpCleanup();
  }

  // Performs a Get and verifies that the key is not found.
  void CheckNotFound(const char* key) { CheckNotFound(Cache(), key); }

  void CheckNotFound(BackendSharedPtr cache, absl::string_view key) {
    InitiateGet(cache, key);
    EXPECT_EQ(DataStatus::NotFound, status_);
  }

  // Initiate a cache Get, and return the Callback* which can be
  // passed to WaitAndCheck or WaitAndCheckNotFound.
  void InitiateGet(absl::string_view key) { return InitiateGet(Cache(), key); }

  void InitiateGet(BackendSharedPtr cache, absl::string_view key) {
    /*
    {
      ScopedMutex lock(mutex_.get());
      ++outstanding_fetches_;
    }
    */
    LookupContextPtr lookup = cache->lookup(makeKey(key));
    lookup->read([this](DataStatus status, const Value& value) {
      value_ = value;
      status_ = status;
      return ReceiverStatus::Ok;
    });
  }

  // Performs a cache Get, waits for callback completion, and checks the
  // result is as expected.
  void CheckGet(absl::string_view key, absl::string_view expected_value) {
    CheckGet(Cache(), key, expected_value);
  }

  void CheckGet(BackendSharedPtr cache, absl::string_view key, absl::string_view expected_value) {
    InitiateGet(cache, key);
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
  CheckPut("Name", "Value");
  CheckGet("Name", "Value");
  // EXPECT_EQ(static_cast<size_t>(9), cache_->size_bytes());  // "Name" + "Value"
  // EXPECT_EQ(static_cast<size_t>(1), cache_->num_elements());
  CheckNotFound("Another Name");

  CheckPut("Name", "NewValue");
  CheckGet("Name", "NewValue");
  // EXPECT_EQ(static_cast<size_t>(12),
  //          cache_->size_bytes());  // "Name" + "NewValue"
  // EXPECT_EQ(static_cast<size_t>(1), cache_->num_elements());

  cache_->remove(makeKey("Name"), nullptr);
  // cache_->SanityCheck();
  Value value_buffer;
  CheckNotFound("Name");
  // EXPECT_EQ(static_cast<size_t>(0), cache_->size_bytes());
  // EXPECT_EQ(static_cast<size_t>(0), cache_->num_elements());
}

/*TEST_F(SimpleCacheTest, RemoveWithPrefix) {
  CheckPut("N1", "Value1");
  CheckPut("N2", "Value2");
  CheckPut("M3", "Value3");
  CheckPut("M4", "Value4");

  // 4*(strlen("N1") + strlen("Value1")) = 4*(2 + 6) = 32
  //EXPECT_EQ(static_cast<size_t>(32), cache_->size_bytes());
  //EXPECT_EQ(static_cast<size_t>(4), cache_->num_elements());

  cache_->removeWithPrefixForTesting("N");
  EXPECT_EQ(static_cast<size_t>(16), cache_->size_bytes());
  EXPECT_EQ(static_cast<size_t>(2), cache_->num_elements());
  CheckNotFound("N1");
  CheckNotFound("N2");
  CheckGet("M3", "Value3");
  CheckGet("M4", "Value4");

  cache_->removeWithPrefixForTesting("M");
  EXPECT_EQ(static_cast<size_t>(0), cache_->size_bytes());
  EXPECT_EQ(static_cast<size_t>(0), cache_->num_elements());
  CheckNotFound("N1");
  CheckNotFound("N2");
  CheckNotFound("M3");
  CheckNotFound("M4");
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
    CheckPut(keys[i], values[i]);
  }
  EXPECT_EQ(kMaxSize, cache_->size_bytes());
  EXPECT_EQ(num_elements, cache_->num_elements());

  // Ensure we can see those.
  for (int i = 0; i < 10; ++i) {
    CheckGet(keys[i], values[i]);
  }

  // Now if we insert a new entry totaling 10 bytes, that should work,
  // but we will lose name0 due to LRU semantics. We should still have name1,
  // and by Get-ing name1 it we will make it the MRU.
  CheckPut("nameA", "valuA");
  CheckGet("nameA", "valuA");
  CheckNotFound("name0");
  CheckGet("name1", "valu1");

  // So now when we put in nameB,valuB we will lose name2 but keep name1,
  // which got bumped up to the MRU when we checked it above.
  CheckPut("nameB", "valuB");
  CheckGet("nameB", "valuB");
  CheckGet("name1", "valu1");
  CheckNotFound("name2");

  // Now insert something 1 byte too big, spelling out "value" this time.
  // We will now lose name3 and name4. We should still have name5-name9,
  // plus name1, nameA, and nameB.
  CheckPut("nameC", "valueC");
  CheckNotFound("name3");
  CheckNotFound("name4");
  CheckGet("nameA", "valuA");
  CheckGet("nameB", "valuB");
  CheckGet("nameC", "valueC");
  CheckGet("name1", "valu1");
  for (int i = 5; i < 10; ++i) {
    CheckGet(keys[i], values[i]);
  }

  // Now the oldest item is "nameA". Freshen it by re-inserting it, tickling
  // the code-path in lru_cache.cc that special-cases handling of re-inserting
  // the same value.
  CheckPut("nameA", "valuA");
  CheckPut("nameD", "valuD");
  // nameB should be evicted, the others should be retained.
  CheckNotFound("nameB");
  CheckGet("nameA", "valuA");
  CheckGet("nameC", "valueC");
  CheckGet("name1", "valu1");
  for (int i = 5; i < 10; ++i) {
    CheckGet(keys[i], values[i]);
  }
  }*/

/*
TEST_F(SimpleCacheTest, BasicInvalid) {
  // Check that we honor callback veto on validity.
  CheckPut("nameA", "valueA");
  CheckPut("nameB", "valueB");
  CheckGet("nameA", "valueA");
  CheckGet("nameB", "valueB");
  set_invalid_value("valueA");
  CheckNotFound("nameA");
  CheckGet("nameB", "valueB");
  }*/

/*TEST_F(SimpleCacheTest, MultiGet) {
  // This covers CacheInterface's default implementation of MultiGet.
  TestMultiGet();
  }*/

TEST_F(SimpleCacheTest, KeyNotFoundWhenUnhealthy) {
  CheckPut("nameA", "valueA");
  cache_->Shutdown(nullptr);
  CheckNotFound("nameA");
}

/*
// Cache starts in 'healthy' state and it should be healthy before
// performing any checks, otherwise Get will return 'not found'
TEST_F(SimpleCacheTest, DoesNotPutWhenUnhealthy) {
  CheckPut("nameA", "valueA");

  cache_->Shutdown(nullptr);
  CheckNotFound("nameA");
}

TEST_F(SimpleCacheTest, DoesNotDeleteWhenUnhealthy) {
  CheckPut("nameA", "valueA");
  cache_->set_is_healthy(false);
  CheckDelete("nameA");

  cache_->set_is_healthy(true);
  CheckGet("nameA", "valueA");
  }

TEST_F(SimpleCacheTest, DoesNotDeleteWithPrefixWhenUnhealthy) {
  CheckPut("nameA", "valueA");
  cache_->set_is_healthy(false);
  cache_->removeWithPrefixForTesting("name");

  cache_->set_is_healthy(true);
  CheckGet("nameA", "valueA");
  }*/

} // namespace Cache
} // namespace Envoy
