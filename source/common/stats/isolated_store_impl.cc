#include "common/stats/isolated_store_impl.h"

#include <string.h>

#include <algorithm>
#include <string>

#include "common/common/utility.h"
#include "common/stats/histogram_impl.h"
#include "common/stats/utility.h"

namespace Envoy {
namespace Stats {

IsolatedStoreImpl::IsolatedStoreImpl()
    : alloc_(symbol_table_),
      counters_(
          [this](const std::string& name) -> CounterSharedPtr {
            std::string tag_extracted_name = name;
            std::vector<Tag> tags;
            return alloc_.makeCounter(name, std::move(tag_extracted_name), std::move(tags));
          },
          alloc_),
      gauges_(
          [this](const std::string& name) -> GaugeSharedPtr {
            std::string tag_extracted_name = name;
            std::vector<Tag> tags;
            return alloc_.makeGauge(name, std::move(tag_extracted_name), std::move(tags));
          },
          alloc_),
      histograms_(
          [this](const std::string& name) -> HistogramSharedPtr {
            return std::make_shared<HistogramImpl>(name, *this, std::string(name),
                                                   std::vector<Tag>(), symbol_table_);
          },
          alloc_) {}

struct IsolatedScopeImpl : public Scope {
  IsolatedScopeImpl(IsolatedStoreImpl& parent, const std::string& prefix)
      : parent_(parent), prefix_(Utility::sanitizeStatsName(prefix))/*,
        counters_(parent.symbolTable().counterPatterns().size()),
        gauges_(parent.symbolTable().gaugePatterns().size()),
        histograms_(parent.symbolTable().histogramPatterns().size())*/ {}

  // Stats::Scope
  ScopePtr createScope(const std::string& name) override {
    return ScopePtr{new IsolatedScopeImpl(parent_, prefix_ + name)};
  }
  void deliverHistogramToSinks(const Histogram&, uint64_t) override {}
  Counter& counter(const std::string& name) override { return parent_.counter(prefix_ + name); }
  Gauge& gauge(const std::string& name) override { return parent_.gauge(prefix_ + name); }
  Histogram& histogram(const std::string& name) override {
    return parent_.histogram(prefix_ + name);
  }
  const Stats::StatsOptions& statsOptions() const override { return parent_.statsOptions(); }

  Counter& getCounter(uint32_t index) override {
    return counter(prefix_ + parent_.symbolTable().counterPatterns().pattern(index));
  }
  Gauge& getGauge(uint32_t index) override {
    return gauge(prefix_ + parent_.symbolTable().gaugePatterns().pattern(index));
  }
  Histogram& getHistogram(uint32_t index) override {
    return histogram(prefix_ + parent_.symbolTable().histogramPatterns().pattern(index));
  }

  IsolatedStoreImpl& parent_;
  const std::string prefix_;

  /*  std::vector<CounterSharedPtr> counters_;
  std::vector<GaugeSharedPtr> gauges_;
  std::vector<HistogramSharedPtr> histograms_;*/
};

ScopePtr IsolatedStoreImpl::createScope(const std::string& name) {
  return ScopePtr{new IsolatedScopeImpl(*this, name)};
}

} // namespace Stats
} // namespace Envoy
