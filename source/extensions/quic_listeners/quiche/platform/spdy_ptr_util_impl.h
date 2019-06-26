#pragma once

#include <memory>

#include "absl/memory/memory.h"

// NOLINT(namespace-envoy)

// This file is part of the QUICHE platform implementation, and is not to be
// consumed or referenced directly by other Envoy code. It serves purely as a
// porting layer for QUICHE.

namespace spdy {

template <typename T, typename... Args> std::unique_ptr<T> SpdyMakeUniqueImpl(Args&&... args) {
  return std::make_unique<T>(std::forward<Args>(args)...);
}

template <typename T> std::unique_ptr<T> SpdyWrapUniqueImpl(T* ptr) {
  return absl::WrapUnique<T>(ptr);
}

} // namespace spdy
