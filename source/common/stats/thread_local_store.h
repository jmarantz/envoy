#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <list>
#include <string>

#include "envoy/thread_local/thread_local.h"

#include "common/common/hash.h"
#include "common/stats/heap_stat_data.h"
#include "common/stats/histogram_impl.h"
#include "common/stats/source_impl.h"
#include "common/stats/symbol_table_impl.h"
#include "common/stats/utility.h"

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "circllhist.h"

namespace Envoy {
namespace Stats {

/**
 * A histogram that is stored in TLS and used to record values per thread. This holds two
 * histograms, one to collect the values and other as backup that is used for merge process. The
 * swap happens during the merge process.
 */
class ThreadLocalHistogramImpl : public Histogram, public MetricImpl {
public:
  ThreadLocalHistogramImpl(StatName name, absl::string_view tag_extracted_name,
                           const std::vector<Tag>& tags, SymbolTable& symbol_table);
  ~ThreadLocalHistogramImpl();

  void merge(histogram_t* target);

  /**
   * Called in the beginning of merge process. Swaps the histogram used for collection so that we do
   * not have to lock the histogram in high throughput TLS writes.
   */
  void beginMerge() {
    // This switches the current_active_ between 1 and 0.
    ASSERT(std::this_thread::get_id() == created_thread_id_);
    current_active_ = otherHistogramIndex();
  }

  // Stats::Histogram
  void recordValue(uint64_t value) override;
  bool used() const override { return flags_ & Flags::Used; }

  // Stats::Metric
  StatName statName() const override { return name_.statName(); }
  SymbolTable& symbolTable() override { return symbol_table_; }
  const SymbolTable& symbolTable() const override { return symbol_table_; }

private:
  uint64_t otherHistogramIndex() const { return 1 - current_active_; }
  uint64_t current_active_;
  histogram_t* histograms_[2];
  std::atomic<uint16_t> flags_;
  std::thread::id created_thread_id_;
  StatNameStorage name_;
  SymbolTable& symbol_table_;
};

typedef std::shared_ptr<ThreadLocalHistogramImpl> TlsHistogramSharedPtr;

class TlsScope;

/**
 * Log Linear Histogram implementation that is stored in the main thread.
 */
class ParentHistogramImpl : public ParentHistogram, public MetricImpl {
public:
  ParentHistogramImpl(StatName name, Store& parent, TlsScope& tlsScope,
                      absl::string_view tag_extracted_name, const std::vector<Tag>& tags);
  ~ParentHistogramImpl();

  void addTlsHistogram(const TlsHistogramSharedPtr& hist_ptr);
  bool used() const override;
  void recordValue(uint64_t value) override;

  /**
   * This method is called during the main stats flush process for each of the histograms. It
   * iterates through the TLS histograms and collects the histogram data of all of them
   * in to "interval_histogram". Then the collected "interval_histogram" is merged to a
   * "cumulative_histogram".
   */
  void merge() override;

  const HistogramStatistics& intervalStatistics() const override { return interval_statistics_; }
  const HistogramStatistics& cumulativeStatistics() const override {
    return cumulative_statistics_;
  }
  const std::string summary() const override;

  // Stats::Metric
  StatName statName() const override { return name_.statName(); }
  SymbolTable& symbolTable() override { return parent_.symbolTable(); }
  const SymbolTable& symbolTable() const override { return parent_.symbolTable(); }

private:
  bool usedLockHeld() const EXCLUSIVE_LOCKS_REQUIRED(merge_lock_);

  Store& parent_;
  TlsScope& tls_scope_;
  histogram_t* interval_histogram_;
  histogram_t* cumulative_histogram_;
  HistogramStatisticsImpl interval_statistics_;
  HistogramStatisticsImpl cumulative_statistics_;
  mutable Thread::MutexBasicLockable merge_lock_;
  std::list<TlsHistogramSharedPtr> tls_histograms_ GUARDED_BY(merge_lock_);
  bool merged_;
  StatNameStorage name_;
};

typedef std::shared_ptr<ParentHistogramImpl> ParentHistogramImplSharedPtr;

/**
 * Class used to create ThreadLocalHistogram in the scope.
 */
class TlsScope : public Scope {
public:
  virtual ~TlsScope() {}

  // TODO(ramaraochavali): Allow direct TLS access for the advanced consumers.
  /**
   * @return a ThreadLocalHistogram within the scope's namespace.
   * @param name name of the histogram with scope prefix attached.
   */
  virtual Histogram& tlsHistogram(StatName name, ParentHistogramImpl& parent) PURE;
};

/**
 * Store implementation with thread local caching. For design details see
 * https://github.com/envoyproxy/envoy/blob/master/docs/stats.md
 */
class ThreadLocalStoreImpl : Logger::Loggable<Logger::Id::stats>, public StoreRoot {
public:
  ThreadLocalStoreImpl(const Stats::StatsOptions& stats_options, StatDataAllocator& alloc);
  ~ThreadLocalStoreImpl();

  // Stats::Scope
  Counter& counterx(StatName name) override { return default_scope_->counterx(name); }
  Counter& counter(const std::string& name) override { return default_scope_->counter(name); }
  ScopePtr createScope(const std::string& name) override;
  void deliverHistogramToSinks(const Histogram& histogram, uint64_t value) override {
    return default_scope_->deliverHistogramToSinks(histogram, value);
  }
  Gauge& gaugex(StatName name) override { return default_scope_->gaugex(name); }
  Gauge& gauge(const std::string& name) override { return default_scope_->gauge(name); }
  Histogram& histogramx(StatName name) override { return default_scope_->histogramx(name); }
  Histogram& histogram(const std::string& name) override { return default_scope_->histogram(name); }
  const SymbolTable& symbolTable() const override { return alloc_.symbolTable(); }
  SymbolTable& symbolTable() override { return alloc_.symbolTable(); }
  const TagProducer& tagProducer() const { return *tag_producer_; }

  // Stats::Store
  std::vector<CounterSharedPtr> counters() const override;
  std::vector<GaugeSharedPtr> gauges() const override;
  std::vector<ParentHistogramSharedPtr> histograms() const override;

  // Stats::StoreRoot
  void addSink(Sink& sink) override { timer_sinks_.push_back(sink); }
  void setTagProducer(TagProducerPtr&& tag_producer) override {
    tag_producer_ = std::move(tag_producer);
  }
  void setStatsMatcher(StatsMatcherPtr&& stats_matcher) override {
    stats_matcher_ = std::move(stats_matcher);
  }
  void initializeThreading(Event::Dispatcher& main_thread_dispatcher,
                           ThreadLocal::Instance& tls) override;
  void shutdownThreading() override;

  void mergeHistograms(PostMergeCb mergeCb) override;

  Source& source() override { return source_; }

  const Stats::StatsOptions& statsOptions() const override { return stats_options_; }
  absl::string_view truncateStatNameIfNeeded(absl::string_view name);

private:
  template <class Stat> using StatMap = StatNameHashMap<Stat>;

  struct TlsCacheEntry {
    StatMap<CounterSharedPtr> counters_;
    StatMap<GaugeSharedPtr> gauges_;
    StatMap<TlsHistogramSharedPtr> histograms_;
    StatMap<ParentHistogramSharedPtr> parent_histograms_;
  };

  struct CentralCacheEntry {
    StatMap<CounterSharedPtr> counters_;
    StatMap<GaugeSharedPtr> gauges_;
    StatMap<ParentHistogramImplSharedPtr> histograms_;
  };

  struct ScopeImpl : public TlsScope {
    ScopeImpl(ThreadLocalStoreImpl& parent, const std::string& prefix);
    ~ScopeImpl();

    // Stats::Scope
    Counter& counterx(StatName name) override;
    void deliverHistogramToSinks(const Histogram& histogram, uint64_t value) override;
    Gauge& gaugex(StatName name) override;
    Histogram& histogramx(StatName name) override;
    Histogram& tlsHistogram(StatName name, ParentHistogramImpl& parent) override;
    const Stats::StatsOptions& statsOptions() const override { return parent_.statsOptions(); }
    ScopePtr createScope(const std::string& name) override {
      return parent_.createScope(prefix_.statName().toString(symbolTable()) + "." + name);
    }
    const SymbolTable& symbolTable() const override { return parent_.symbolTable(); }
    SymbolTable& symbolTable() override { return parent_.symbolTable(); }

    Counter& counter(const std::string& name) override {
      StatNameTempStorage storage(name, symbolTable());
      return counterx(storage.statName());
    }
    Gauge& gauge(const std::string& name) override {
      StatNameTempStorage storage(name, symbolTable());
      return gaugex(storage.statName());
    }
    Histogram& histogram(const std::string& name) override {
      StatNameTempStorage storage(name, symbolTable());
      return histogramx(storage.statName());
    }

    template <class StatType>
    using MakeStatFn =
        std::function<std::shared_ptr<StatType>(StatDataAllocator&, StatName name,
                                                absl::string_view tag_extracted_name,
                                                const std::vector<Tag>& tags)>;

    /**
     * Makes a stat either by looking it up in the central cache,
     * generating it from the the parent allocator, or as a last
     * result, creating it with the heap allocator.
     *
     * @param name the full name of the stat (not tag extracted).
     * @param central_cache_map a map from name to the desired object in the central cache.
     * @param make_stat a function to generate the stat object, called if it's not in cache.
     * @param tls_ref possibly null reference to a cache entry for this stat, which will be
     *     used if non-empty, or filled in if empty (and non-null).
     */
    template <class StatType>
    StatType&
    safeMakeStat(StatName name, StatMap<std::shared_ptr<StatType>>& central_cache_map,
                 MakeStatFn<StatType> make_stat, StatMap<std::shared_ptr<StatType>>* tls_cache);

    void extractTagsAndTruncate(
        StatName& name, std::unique_ptr<StatNameTempStorage>& truncated_name_storage,
        std::vector<Tag>& tags,
        std::string& tag_extracted_name);

    static std::atomic<uint64_t> next_scope_id_;

    const uint64_t scope_id_;
    ThreadLocalStoreImpl& parent_;
    StatNameStorage prefix_;
    CentralCacheEntry central_cache_;
  };

  struct TlsCache : public ThreadLocal::ThreadLocalObject {
    // The TLS scope cache is keyed by scope ID. This is used to avoid complex circular references
    // during scope destruction. An ID is required vs. using the address of the scope pointer
    // because it's possible that the memory allocator will recyle the scope pointer immediately
    // upon destruction, leading to a situation in which a new scope with the same address is used
    // to reference the cache, and then subsequently cache flushed, leaving nothing in the central
    // store. See the overview for more information. This complexity is required for lockless
    // operation in the fast path.
    absl::flat_hash_map<uint64_t, TlsCacheEntry> scope_cache_;
  };

  std::string getTagsForName(const std::string& name, std::vector<Tag>& tags) const;
  void clearScopeFromCaches(uint64_t scope_id);
  void releaseScopeCrossThread(ScopeImpl* scope);
  void mergeInternal(PostMergeCb mergeCb);

  const Stats::StatsOptions& stats_options_;
  StatDataAllocator& alloc_;
  Event::Dispatcher* main_thread_dispatcher_{};
  ThreadLocal::SlotPtr tls_;
  mutable Thread::MutexBasicLockable lock_;
  absl::flat_hash_set<ScopeImpl*> scopes_ GUARDED_BY(lock_);
  ScopePtr default_scope_;
  std::list<std::reference_wrapper<Sink>> timer_sinks_;
  TagProducerPtr tag_producer_;
  StatsMatcherPtr stats_matcher_;
  std::atomic<bool> shutting_down_{};
  std::atomic<bool> merge_in_progress_{};
  StatNameStorage stats_overflow_;
  Counter& num_last_resort_stats_;
  HeapStatDataAllocator heap_allocator_;
  SourceImpl source_;

  NullCounterImpl null_counter_;
  NullGaugeImpl null_gauge_;
  NullHistogramImpl null_histogram_;
};

} // namespace Stats
} // namespace Envoy
