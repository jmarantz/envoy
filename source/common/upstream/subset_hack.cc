#include "common/upstream/subset_hack.h"

#include <iostream>
#include <random>
#include <openssl/hmac.h>

#include "common/common/assert.h"
#include "common/common/hash.h"
#include "common/common/utility.h"

static bool is_enabled = false;
const uint64_t kuint64max = 0xffffffffffffffff;

enum class HashChoice { XX, Sha1, Absl };
enum class Strategy { Xor, Divide, Modulus /*, Twister*/ };
//enum class XorBits { Four, Eight };
//enum class XorTactic { Reverse, FourBit, ReverseFourBit, EightBit };

#define REVERSE 0
#define USE_XOR_4_BIT 0

#define STRATEGY Strategy::Xor
#define HASHER HashChoice::Absl

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
  return dist(gen) > subset_hack->fraction_255_;

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
  subset_hack->fraction64_ = fraction_to_allow * kuint64max;
  subset_hack->shard_identifier_ = std::string(shard_identifier);
  subset_hack->shard_hash_ = hash(shard_identifier);
  ASSERT(fraction_to_allow <= 1.0);
  ASSERT(fraction_to_allow >= 0.0);
  switch (STRATEGY) {
    case Strategy::Divide:
      break;
    case Strategy::Modulus:
      subset_hack->fraction_255_ = ((*primes)[0] - 1) * fraction_to_allow; // 1.0 --> 1010, 0 --> 0.
      break;
    case Strategy::Xor:
#if USE_XOR_4_BIT
      subset_hack->fraction_255_ = 15 * fraction_to_allow; // 1.0 --> 15, 0 --> 0.
#else
      subset_hack->fraction_255_ = 255 * fraction_to_allow; // 1.0 --> 255, 0 --> 0.
#endif
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
  if (reverse_bits) {
#if REVERSE
    b = reverseBits(b, num_bits);
#endif
  }
  a = (a >> num_bits) ^ b;
  return a & ((uint64_t(1) << num_bits) - 1);
}

static uint64_t hashCombine(SubsetHack* subset_hack, absl::string_view host) {
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
      return subset_hack->shard_hash_ ^ hash(host);
    case HashChoice::Absl: {
      using IntStringViewPair = std::pair<uint64_t, absl::string_view>;
      absl::Hash<IntStringViewPair> hasher;
      return hasher(IntStringViewPair(subset_hack->shard_hash_, host));
    }
  }
}

bool SubsetHack::skipHost(absl::string_view host) {
  if (!is_enabled) {
    return false;
  }

  SubsetHack* subset_hack = getInstance();

  switch (STRATEGY) {
    case Strategy::Divide: return hashCombine(subset_hack, host) > subset_hack->fraction64_;
    case Strategy::Modulus: {
      uint64_t hash = hashCombine(subset_hack, host);
      for (int32_t i = primes->size() - 1; i >= 0; --i) {
        if (i > 0) {
          ASSERT((*primes)[i - 1] < (*primes)[i]);
        }
        hash = hash % (*primes)[i];
      }
      return hash > subset_hack->fraction_255_;
    }
    case Strategy::Xor:
      uint64_t host_hash = hash(host);
      uint64_t reduce_64 = host_hash ^ subset_hack->shard_hash_;
      uint32_t reduce_32 = reduce(reduce_64, 32, false);
      uint32_t reduce_16 = reduce(reduce_32, 16, true);
      uint32_t reduce_8 = reduce(reduce_16, 8, false);

# if USE_XOR_4_BIT
      uint32_t reduce_4 = reduce(reduce_8, 4, true);
      return reduce_4 > subset_hack->fraction_255_;
# else
      return reduce_8 > subset_hack->fraction_255_;
# endif
  }
}

} // namespace Upstream
} // namespace Envoy
