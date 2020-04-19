#pragma once

#include "absl/strings/string_view.h"

namespace Envoy {
namespace Upstream {

class SubsetHack {
 public:
  enum class HashChoice { XX, Sha1, Absl, AbslCombine };
  enum class Strategy { Xor, XorReverse, Divide, Modulus, ByteCombine /*, Twister*/ };

  SubsetHack(HashChoice hash_choice, Strategy strategy, uint32_t xor_bits,
             uint32_t prime, absl::string_view shard_identifier,
             double fraction_to_allow);

  bool skipHost(absl::string_view host);
  uint64_t hash(absl::string_view key) const;

 private:
  uint64_t reduce(uint64_t a, uint32_t num_bits) const;
  static uint32_t reverseBits(uint32_t data, uint32_t num_bits);
  uint64_t hashCombine(absl::string_view host) const;

  const HashChoice hash_choice_;
  const Strategy strategy_;
  const uint32_t xor_bits_;
  const uint32_t prime_;
  const uint64_t shard_hash_;
  uint64_t fraction_{0};
  const std::string shard_identifier_;
};

} // namespace Upstream
} // namespace Envoy
