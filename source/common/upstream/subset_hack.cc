#include "common/upstream/subset_hack.h"

#include <iostream>
#include <random>
#include <openssl/hmac.h>

#include "common/common/assert.h"
#include "common/common/hash.h"
#include "common/common/utility.h"

static bool is_enabled = false;
const uint64_t kuint16max = 0xffff;
const uint64_t kuint32max = 0xffffffff;
const uint64_t kuint64max = 0xffffffffffffffff;

enum class HashChoice { XX, Sha1, Absl };
enum class Strategy { Xor, Divide, Modulus, ByteCombine /*, Twister*/ };

#define REVERSE 1

#define STRATEGY Strategy::Xor
#define HASHER HashChoice::XX
#define XOR_BITS 16

namespace Envoy {
namespace Upstream {

static std::vector<uint64_t>* primes = nullptr;

static SubsetHack* getInstance() {
  static SubsetHack* subset_hack = new SubsetHack;

  if (primes == nullptr) {
    primes = new std::vector<uint64_t>;

    std::cout << "primes i: ";
    for (uint32_t i = 255; i < 1*1000*1000*1000; i *= 1009) {
      primes->push_back(Primes::findPrimeLargerThan(i));
      std::cout << primes->back() << " ";
    }
    std::cout << std::endl;
  }
  return subset_hack;
}

bool SubsetHack::enabled() { return is_enabled; }

/*#if STRATEGY == Strategy::Twister
uint64_t swapBits(uint64_t bits) {
  std::mt19937 gen(bits);
  for (int i = 63; i >= 0; ++i) {
    std::uniform_int_distribution<uint64_t> dist(0, i - 1);
    uint64_t bit = dist(gen);
    uint64_t value_at_bit = (bits >> bit) & 1;
    uint64_t value_at_i = (bits >> i) & 1;
    if (value_at_bit != value_at_i) {
      if (value_at_bit) {
        bits |= (1 << i);
        bits &= ~(uint64_t(1 << bit));
      } else {
        bits |= (1 << bit);
        bits &= ~(uint64_t(1 << i));
      }
    }
  }
  return bits;
}
  std::mt19937_64 gen(hash);
  std::uniform_int_distribution<uint32_t> dist(0, 255);
  return dist(gen) > subset_hack->fraction_;

#endif*/

uint64_t hashSha1(absl::string_view data) {
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

static uint64_t hash(absl::string_view key) {
  switch (HASHER) {
    case HashChoice::XX: return HashUtil::xxHash64(key);
    case HashChoice::Absl: return absl::Hash<absl::string_view>()(key);
    case HashChoice::Sha1: return hashSha1(key);
  }
}

void SubsetHack::enableSubsetting(absl::string_view shard_identifier, double fraction_to_allow) {
  SubsetHack* subset_hack = getInstance();
  subset_hack->shard_identifier_ = std::string(shard_identifier);
  subset_hack->shard_hash_ = hash(shard_identifier);
  ASSERT(fraction_to_allow <= 1.0);
  ASSERT(fraction_to_allow >= 0.0);
  switch (STRATEGY) {
    case Strategy::Divide:
      subset_hack->fraction_ = fraction_to_allow * kuint64max;
      break;
    case Strategy::Modulus:
    case Strategy::ByteCombine:
      subset_hack->fraction_ = ((*primes)[0] - 1) * fraction_to_allow; // 1.0 --> 1010, 0 --> 0.
      break;
    case Strategy::Xor:
      switch (XOR_BITS) {
        case 64: subset_hack->fraction_ = fraction_to_allow * kuint64max; break;
        case 32: subset_hack->fraction_ = fraction_to_allow * kuint32max; break;
        case 16: subset_hack->fraction_ = fraction_to_allow * kuint16max; break;
        case 8: subset_hack->fraction_ = fraction_to_allow * 255; break;
        case 4: subset_hack->fraction_ = fraction_to_allow * 15; break; // 1.0 --> 15, 0 --> 0.
        default:
          RELEASE_ASSERT(false, "only 4/8/16/32/64 allowed");
          break;
      }
  }
  is_enabled = true;
}

void SubsetHack::disable() {
  is_enabled = false;
}

uint32_t reverseBits(uint32_t data, uint32_t num_bits) {
  uint32_t out = 0;
  for (uint32_t i = 0, j = num_bits - 1; i < num_bits; ++i, --j) {
    uint32_t bit = data & (1 << i);
    ASSERT(j < num_bits);  // no wraparound.
    out |= (bit != 0) << j;
  }
  return out;
}

static inline uint64_t reduce(uint64_t a, uint32_t num_bits, bool reverse_bits) {
  uint32_t b = a;
  a = a >> num_bits;
  if (reverse_bits) {
#if REVERSE
    //if ((a & 1) ^ (b & 1)) {
    //  a = reverseBits(a, num_bits);
    //} else {
    b = reverseBits(b, num_bits);
    //}
#endif
  }
  a ^= b;
  return a & ((uint64_t(1) << num_bits) - 1);
}

static uint64_t hashCombine(SubsetHack* subset_hack, absl::string_view host) {
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
      return subset_hack->shard_hash_ ^ hash(host);
#if 0
    case HashChoice::Absl: {
      using IntStringViewPair = std::pair<uint64_t, absl::string_view>;
      absl::Hash<IntStringViewPair> hasher;
      return hasher(IntStringViewPair(subset_hack->shard_hash_, host));
    }
  }
#endif
}

bool SubsetHack::skipHost(absl::string_view host) {
  if (!is_enabled) {
    return false;
  }

  SubsetHack* subset_hack = getInstance();

  switch (STRATEGY) {
    case Strategy::Divide: return hashCombine(subset_hack, host) > subset_hack->fraction_;
    case Strategy::Modulus: {
      uint64_t hash = hashCombine(subset_hack, host);
      for (int32_t i = primes->size() - 1; i >= 0; --i) {
        if (i > 0) {
          ASSERT((*primes)[i - 1] < (*primes)[i]);
        }
        hash = hash % (*primes)[i];
      }
      return hash > subset_hack->fraction_;
    }
    case Strategy::Xor: {
      // uint64_t accum = hash(host) ^ subset_hack->shard_hash_;
      uint64_t accum = hashCombine(subset_hack, host);
      for (uint32_t bits = 32; bits >= XOR_BITS; bits >>= 1) {
        accum = reduce(accum, bits, (bits == 16));
      }
      return accum > subset_hack->fraction_;
    }
    case Strategy::ByteCombine:
      int32_t accum = subset_hack->shard_hash_;
      for (char c : host) {
        accum = (accum << 4) + c;
        int32_t g = accum & 0xF0000000L;
        if (g != 0) {
          accum ^= g >> 24;
        }
        accum &= ~g;
      }
      return (accum % primes->at(0)) > subset_hack->fraction_;
  }
}

} // namespace Upstream
} // namespace Envoy
