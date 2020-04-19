#include "common/upstream/subset_hack.h"

#include <iostream>
#include <random>
#include <openssl/hmac.h>

#include "common/common/assert.h"
#include "common/common/hash.h"
#include "common/common/utility.h"

const uint64_t kuint16max = 0xffff;
const uint64_t kuint32max = 0xffffffff;
const uint64_t kuint64max = 0xffffffffffffffff;

enum class HashChoice { XX, Sha1, Absl };
enum class Strategy { Xor, Divide, Modulus, ByteCombine /*, Twister*/ };

namespace Envoy {
namespace Upstream {

//static const uint32_t GoodPrime = 257297;
static const uint32_t GoodPrime = 259610677;

static uint64_t hashSha1(absl::string_view data) {
  absl::string_view key("my hash key");
  unsigned char buf[20];
  unsigned int signature_length = sizeof(buf);
  void* md = HMAC(EVP_sha1(), key.data(), key.size(),
                  reinterpret_cast<const unsigned char*>(data.data()),
                  data.size(), buf, &signature_length);
  ASSERT(signature_length >= sizeof(uint64_t));
  uint64_t hash = *reinterpret_cast<const uint64_t*>(md);
  //std::cout << "hash: " << hash << std::endl;
  return hash;
}

uint64_t SubsetHack::hash(absl::string_view key) const {
  switch (hash_choice_) {
    case HashChoice::XX: return HashUtil::xxHash64(key);
    case HashChoice::Absl: return absl::Hash<absl::string_view>()(key);
    case HashChoice::Sha1: return hashSha1(key);
  }
}

uint32_t SubsetHack::reverseBits(uint32_t data, uint32_t num_bits) {
  uint32_t out = 0;
  for (uint32_t i = 0, j = num_bits - 1; i < num_bits; ++i, --j) {
    uint32_t bit = data & (1 << i);
    ASSERT(j < num_bits);  // no wraparound.
    out |= (bit != 0) << j;
  }
  return out;
}

uint64_t SubsetHack::reduce(uint64_t a, uint32_t num_bits) const {
  uint32_t b = a;
  a = a >> num_bits;
  if (num_bits == 16 && strategy_ == Strategy::XorReverse) {
    //if ((a & 1) ^ (b & 1)) {
    //  a = reverseBits(a, num_bits);
    //} else {
    b = reverseBits(b, num_bits);
    //}
  }
  a ^= b;
  return a & ((uint64_t(1) << num_bits) - 1);
}

uint64_t SubsetHack::hashCombine(absl::string_view host) const {
#if 0
  switch (HASHER) {
    case HashChoice::XX:
    case HashChoice::Sha1:
      /*
      Envoy::HashCombiner combiner;
      combiner.append(subset_hack->shard_identifier_.data(), subset_hack->shard_identifier_.size());
      combiner.append("\n", 1);
      combiner.append(host.data(), host.size());
      return combiner.hash();
      return HashUtil::xxHash64(absl::StrCat(subset_hack->shard_identifier_, "\n", host));
      //return subset_hack->shard_hash_ + 31 * HashUtil::xxHash64(host);
      */
#endif
      return shard_hash_ ^ hash(host);
#if 0
    case HashChoice::Absl: {
      using IntStringViewPair = std::pair<uint64_t, absl::string_view>;
      absl::Hash<IntStringViewPair> hasher;
      return hasher(IntStringViewPair(subset_hack->shard_hash_, host));
    }
  }
#endif
}

SubsetHack::SubsetHack(HashChoice hash_choice, Strategy strategy, uint32_t xor_bits,
                       absl::string_view shard_identifier, double fraction_to_allow)
    : hash_choice_(hash_choice),
      strategy_(strategy),
      xor_bits_(xor_bits),
      shard_hash_(hash(shard_identifier)),
      shard_identifier_(std::string(shard_identifier)) {
  ASSERT(fraction_to_allow <= 1.0);
  ASSERT(fraction_to_allow >= 0.0);
  switch (strategy) {
    case Strategy::Divide:
      fraction_ = fraction_to_allow * kuint64max;
      break;
    case Strategy::Modulus:
    case Strategy::ByteCombine: {
      fraction_ = (GoodPrime - 1) * fraction_to_allow; // 1.0 --> 1010, 0 --> 0.
      break;
    }
    case Strategy::Xor:
    case Strategy::XorReverse:
      switch (xor_bits_) {
        case 64: fraction_ = fraction_to_allow * kuint64max; break;
        case 32: fraction_ = fraction_to_allow * kuint32max; break;
        case 16: fraction_ = fraction_to_allow * kuint16max; break;
        case 8: fraction_ = fraction_to_allow * 255; break;
        case 4: fraction_ = fraction_to_allow * 15; break; // 1.0 --> 15, 0 --> 0.
        default:
          RELEASE_ASSERT(false, "only 4/8/16/32/64 allowed");
          break;
      }
  }
}

bool SubsetHack::skipHost(absl::string_view host) {
  switch (strategy_) {
    case Strategy::Divide: return hashCombine(host) > fraction_;
    case Strategy::Modulus: {
      uint64_t hash = hashCombine(host) % GoodPrime;
      return hash > fraction_;
    }
    case Strategy::XorReverse:
    case Strategy::Xor: {
      // uint64_t accum = hash(host) ^ shard_hash_;
      uint64_t accum = hashCombine(host);
      for (uint32_t bits = 32; bits >= xor_bits_; bits >>= 1) {
        accum = reduce(accum, bits);
      }
      return accum > fraction_;
    }
    case Strategy::ByteCombine:
      int32_t accum = shard_hash_;
      for (char c : host) {
        accum = (accum << 4) + c;
        int32_t g = accum & 0xF0000000L;
        if (g != 0) {
          accum ^= g >> 24;
        }
        accum &= ~g;
      }
      return (accum % GoodPrime) > fraction_;
  }
}

} // namespace Upstream
} // namespace Envoy
