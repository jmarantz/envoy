#pragma once

#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"

namespace Envoy {
namespace Stats {
namespace TestUtil {

// We can only test absolute memory usage if the malloc library is a known
// quantity. This decision is centralized here.
#ifdef TCMALLOC
#define ENABLE_MEMORY_USAGE_TESTS
#endif

/**
 * Calls a fn with every stat name in the system for the given number of
 * clusters, as of Oct 12, 2018.
 *
 * @param num_clusters the number of clusters for which to generate stats.
 * @param fn the function to call with every stat name.
 */
void foreachStat(int num_clusters, std::function<void(absl::string_view)> fn);

} // namespace TestUtil
} // namespace Stats
} // namespace Envoy
