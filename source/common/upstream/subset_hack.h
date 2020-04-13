#pragma once

#include "absl/strings/string_view.h"

namespace Envoy {
namespace Upstream {

class SubsetHack {
 public:
  static bool enabled();
  static bool skipHost(absl::string_view host);
  static void enableSubsetting(absl::string_view shard_identifier, double fraction_to_allow);
  void disable();
  static uint32_t reverseBits(uint32_t data, uint32_t num_bits);

  uint64_t shard_hash_{0};
  uint64_t fraction_{0};
  std::string shard_identifier_;
};

} // namespace Upstream
} // namespace Envoy
