#include "common/cache/simple_cache.h"
#include "common/common/utility.h"

#include "absl/strings/str_cat.h"
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
    CacheInterfaceSharedPtrSharedPtr cache = cache_;
    cache_ = nullptr;
    if (cache->isHealthy()) {
      cache->shutdown(nullptr);
    }
  }

  // Writes a value into the cache.
  void checkPut(const std::string& key, const std::string& value) { checkPut(cache(), key, value); }

  void checkPut(CacheInterfaceSharedPtrSharedPtr cache, const std::string& key,
                absl::string_view value) {
    InsertContextPtr inserter = cache->insert(makeDescriptor(key));
    inserter->write(makeValue(value), nullptr);
    PostOpCleanup();
  }

  void checkRemove(const std::string& key) {
    cache()->remove(makeDescriptor(key), nullptr);
    PostOpCleanup();
  }

  // Performs a Get and verifies that the key is not found.
  void checkNotFound(const char* key) { checkNotFound(cache(), key); }

  void checkNotFound(CacheInterfaceSharedPtrSharedPtr cache, absl::string_view key) {
    initiateGet(cache, key);
    EXPECT_EQ(DataStatus::NotFound, status_);
  }

  // Initiate a cache Get, and return the Callback* which can be
  // passed to WaitAndCheck or WaitAndcheckNotFound.
  void initiateGet(absl::string_view key) { return initiateGet(cache(), key); }

  void initiateGet(CacheInterfaceSharedPtrSharedPtr cache, absl::string_view key) {
    /*
    {
      ScopedMutex lock(mutex_.get());
      ++outstanding_fetches_;
    }
    */
    value_.reset();
    LookupContextPtr lookup = cache->lookup(makeDescriptor(key));
    nextChunk(std::move(lookup));
  }

  void nextChunk(LookupContextPtr lookup) {
    lookup->read([this, &lookup](DataStatus status, const Value& value) {
      if (ValidStatus(status)) {
        if ((status == DataStatus::LastChunk) && (value_.get() == nullptr)) {
          value_ = value; // Zero-copy share value if it came in one chunk.
        } else {
          if (value_.get() == nullptr) {
            value_ = Cache::makeValue();
          }
          absl::StrAppend(&value_->value_, value->value_);
        }
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
    checkGet(cache(), key, expected_value);
  }

  void checkGet(CacheInterfaceSharedPtrSharedPtr cache, absl::string_view key,
                absl::string_view expected_value) {
    initiateGet(cache, key);
    EXPECT_EQ(expected_value, value_->value_);
  }

  CacheInterfaceSharedPtrSharedPtr cache() { return cache_; }
  void PostOpCleanup() { /*cache_->SanityCheck();*/
  }

  Descriptor makeDescriptor(absl::string_view key) {
    Descriptor desc{key, current_time_, attributes_};
    return desc;
  };

  Value makeValue(absl::string_view val) {
    Value value = Cache::makeValue();
    value->value_ = std::string(val);
    value->timestamp_ = current_time_;
    return value;
  }

  void testMultiGet() {
    populateCache(2);
    DescriptorVec descs({makeDescriptor("n0"), makeDescriptor("n1"), makeDescriptor("not found")});
    LookupContextVec lookups = cache()->multiLookup(descs);
    struct {
      const char* value;
      DataStatus status;
    } expected_results[] = {
        {"v0", DataStatus::LastChunk}, {"v1", DataStatus::LastChunk}, {"", DataStatus::NotFound}};
    for (size_t i = 0; i < lookups.size(); ++i) {
      value_ = Cache::makeValue();
      nextChunk(std::move(lookups[i]));
      EXPECT_EQ(expected_results[i].value, value_->value_);
      EXPECT_EQ(expected_results[i].status, status_);
    }
  }

  // Populates the cache with keys in pattern n0 n1 n2 n3...
  // and values in pattern v0 v1 v2 v3...
  void populateCache(int num) {
    for (int i = 0; i < num; ++i) {
      checkPut(absl::StrCat("n", i), absl::StrCat("v", i));
    }
  }

  CacheInterfaceSharedPtrSharedPtr cache_;
  Value value_;
  AttributeMap attributes_;
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

  cache_->remove(makeDescriptor("Name"), nullptr);
  // cache_->SanityCheck();
  Value value_buffer;
  checkNotFound("Name");
  // EXPECT_EQ(static_cast<size_t>(0), cache_->size_bytes());
  // EXPECT_EQ(static_cast<size_t>(0), cache_->num_elements());
}

TEST_F(SimpleCacheTest, StreamingPut) {
  Descriptor key = makeDescriptor("key");
  InsertContextPtr inserter = cache_->insert(key);
  inserter->write(makeValue("Hello, "), [this, &key, &inserter](bool) {
    // While we are string in the value, the cache reponds with InsertInProgress
    LookupContextPtr lookup = cache_->lookup(key);
    lookup->read([](DataStatus status, const Value&) -> ReceiverStatus {
      EXPECT_EQ(DataStatus::InsertInProgress, status);
      return ReceiverStatus::Ok;
    });
    inserter->write(makeValue("World!"), nullptr);
  });
  checkGet("key", "Hello, World!");
  PostOpCleanup();
}

TEST_F(SimpleCacheTest, StreamingGet) {
  checkPut("Name", "Value");
  attributes_["split"] = "true";
  checkGet("Name", "Value");
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

TEST_F(SimpleCacheTest, MultiGet) {
  // This covers CacheInterface's default implementation of MultiGet.
  testMultiGet();
}

TEST_F(SimpleCacheTest, DescriptorNotFoundWhenUnhealthy) {
  checkPut("nameA", "valueA");
  cache_->shutdown(nullptr);
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
