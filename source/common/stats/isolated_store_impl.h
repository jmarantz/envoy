#pragma once

#include <string.h>

#include <algorithm>
#include <string>

#include "envoy/stats/stats.h"
#include "envoy/stats/stats_options.h"
#include "envoy/stats/store.h"

#include "common/common/utility.h"
#include "common/stats/heap_stat_data.h"
#include "common/stats/stat_name_ref.h"
#include "common/stats/stats_options_impl.h"
#include "common/stats/symbol_table_impl.h"
#include "common/stats/utility.h"

#include "absl/container/flat_hash_map.h"

namespace Envoy {
namespace Stats {

/**
 * A stats cache template that is used by the isolated store.
 */
template <class Base> class IsolatedStatsCache {
public:
  typedef std::function<std::shared_ptr<Base>(const std::string& name)> Allocator;

  IsolatedStatsCache(Allocator alloc, HeapStatDataAllocator& heap_alloc)
      : alloc_(alloc), heap_alloc_(heap_alloc), stats_(
            defaultBucketCount(), StatNameRefHash(heap_alloc.symbolTable()),
            StatNameRefCompare(heap_alloc.symbolTable()))
  {}

  Base& get(const std::string& name) {
    StatNameRef name_ref(name);
    auto stat = stats_.find(name_ref);
    if (stat != stats_.end()) {
      return *stat->second;
    }

    std::shared_ptr<Base> new_stat = alloc_(name);
    stats_.emplace(new_stat->nameRef(), new_stat);
    return *new_stat;
  }

  std::vector<std::shared_ptr<Base>> toVector() const {
    std::vector<std::shared_ptr<Base>> vec;
    vec.reserve(stats_.size());
    for (auto& stat : stats_) {
      vec.push_back(stat.second);
    }

    return vec;
  }

  static size_t defaultBucketCount() {
    // absl::flat_hash_map<int, int> map;
    // return map.bucket_count();
    return 0;
  }

private:
  using StatMap = absl::flat_hash_map<StatNameRef, std::shared_ptr<Base>, StatNameRefHash,
                                      StatNameRefCompare>;
  Allocator alloc_;
  HeapStatDataAllocator& heap_alloc_;
  StatMap stats_;
};

class IsolatedStoreImpl : public Store {
public:
  IsolatedStoreImpl();

  // Stats::Scope
  Counter& counter(const std::string& name) override { return counters_.get(name); }
  ScopePtr createScope(const std::string& name) override;
  void deliverHistogramToSinks(const Histogram&, uint64_t) override {}
  Gauge& gauge(const std::string& name) override { return gauges_.get(name); }
  Histogram& histogram(const std::string& name) override {
    Histogram& histogram = histograms_.get(name);
    return histogram;
  }
  const Stats::StatsOptions& statsOptions() const override { return stats_options_; }
  const SymbolTable& symbolTable() const { return symbol_table_; }

  // Stats::Store
  std::vector<CounterSharedPtr> counters() const override { return counters_.toVector(); }
  std::vector<GaugeSharedPtr> gauges() const override { return gauges_.toVector(); }
  std::vector<ParentHistogramSharedPtr> histograms() const override {
    return std::vector<ParentHistogramSharedPtr>{};
  }

private:
  SymbolTable symbol_table_;
  HeapStatDataAllocator alloc_;
  IsolatedStatsCache<Counter> counters_;
  IsolatedStatsCache<Gauge> gauges_;
  IsolatedStatsCache<Histogram> histograms_;
  const StatsOptionsImpl stats_options_;
};

} // namespace Stats
} // namespace Envoy
