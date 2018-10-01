#include "common/stats/thread_local_store.h"

#include <chrono>
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>

#include "envoy/stats/histogram.h"
#include "envoy/stats/sink.h"
#include "envoy/stats/scope.h"
#include "envoy/stats/stat_data_allocator.h"
#include "envoy/stats/stats.h"
#include "envoy/stats/stats_options.h"

#include "common/common/lock_guard.h"
#include "common/stats/tag_producer_impl.h"

#include "absl/strings/str_join.h"

#define ENABLE_TLS_CACHE true

namespace Envoy {
namespace Stats {

ThreadLocalStoreImpl::ThreadLocalStoreImpl(const StatsOptions& stats_options,
                                           StatDataAllocator& alloc)
    : stats_options_(stats_options), alloc_(alloc), symbol_table_(alloc.symbolTable()),
      heap_allocator_(symbol_table_), default_scope_(createScope("")),
      tag_producer_(std::make_unique<TagProducerImpl>()),
      num_last_resort_stats_(default_scope_->counter("stats.overflow")), source_(*this) {
}

ThreadLocalStoreImpl::~ThreadLocalStoreImpl() {
  ASSERT(shutting_down_);
  default_scope_.reset();
  ASSERT(scopes_.empty());
}

std::vector<CounterSharedPtr> ThreadLocalStoreImpl::counters() const {
  // Handle de-dup due to overlapping scopes.
  std::vector<CounterSharedPtr> ret;
  Thread::LockGuard lock(lock_);
  StatRefSet names(defaultBucketCount(), StatNameRefStarHash(symbol_table_),
                   StatNameRefStarCompare(symbol_table_));
  //StatSet<CounterSharedPtr> names;
  for (ScopeImpl* scope : scopes_) {
    for (auto& counter : scope->central_cache_.counters_) {
      const StatNameRef& name_ref = counter.first;
      if (names.insert(&name_ref).second) {
        ret.push_back(counter.second);
      }
      // if (names.insert(counter).second) {
      //   ret.push_back(counter);
      // }
    }
  }

  return ret;
}

ScopePtr ThreadLocalStoreImpl::createScope(const std::string& name) {
  std::unique_ptr<ScopeImpl> new_scope(new ScopeImpl(*this, name));
  Thread::LockGuard lock(lock_);
  scopes_.emplace(new_scope.get());
  return std::move(new_scope);
}

std::vector<GaugeSharedPtr> ThreadLocalStoreImpl::gauges() const {
  // Handle de-dup due to overlapping scopes.
  std::vector<GaugeSharedPtr> ret;
  Thread::LockGuard lock(lock_);
  StatRefSet names(defaultBucketCount(), StatNameRefStarHash(symbol_table_),
                   StatNameRefStarCompare(symbol_table_));
  //StatSet<GaugeSharedPtr> names;
  for (ScopeImpl* scope : scopes_) {
    for (auto& gauge : scope->central_cache_.gauges_) {
      const StatNameRef& name_ref = gauge.first;
      if (names.insert(&name_ref).second) {
        ret.push_back(gauge.second);
      }
      //      if (names.insert(gauge).second) {
      //        ret.push_back(gauge);
      //      }
    }
  }

  return ret;
}

std::vector<ParentHistogramSharedPtr> ThreadLocalStoreImpl::histograms() const {
  std::vector<ParentHistogramSharedPtr> ret;
  Thread::LockGuard lock(lock_);
  // TODO(ramaraochavali): As histograms don't share storage, there is a chance of duplicate names
  // here. We need to create global storage for histograms similar to how we have a central storage
  // in shared memory for counters/gauges. In the interim, no de-dup is done here. This may result
  // in histograms with duplicate names, but until shared storage is implemented it's ultimately
  // less confusing for users who have such configs.
  for (ScopeImpl* scope : scopes_) {
    for (const auto& name_histogram_pair : scope->central_cache_.histograms_) {
      const ParentHistogramSharedPtr& parent_hist = name_histogram_pair.second;
      ret.push_back(parent_hist);
    }
  }

  return ret;
}

void ThreadLocalStoreImpl::initializeThreading(Event::Dispatcher& main_thread_dispatcher,
                                               ThreadLocal::Instance& tls) {
  main_thread_dispatcher_ = &main_thread_dispatcher;
  tls_ = tls.allocateSlot();
  tls_->set([this](Event::Dispatcher&) -> ThreadLocal::ThreadLocalObjectSharedPtr {
              return std::make_shared<TlsCache>(symbol_table_);
  });
}

void ThreadLocalStoreImpl::shutdownThreading() {
  // This will block both future cache fills as well as cache flushes.
  shutting_down_ = true;
}

void ThreadLocalStoreImpl::mergeHistograms(PostMergeCb merge_complete_cb) {
  if (!shutting_down_) {
    ASSERT(!merge_in_progress_);
    merge_in_progress_ = true;
    tls_->runOnAllThreads(
        [this]() -> void {
          for (const auto& scope : tls_->getTyped<TlsCache>().scope_cache_) {
            const TlsCacheEntry& tls_cache_entry = *scope.second;
            for (const auto& name_histogram_pair : tls_cache_entry.histograms_) {
              const TlsHistogramSharedPtr& tls_hist = name_histogram_pair.second;
              tls_hist->beginMerge();
            }
          }
        },
        [this, merge_complete_cb]() -> void { mergeInternal(merge_complete_cb); });
  } else {
    // If server is shutting down, just call the callback to allow flush to continue.
    merge_complete_cb();
  }
}

void ThreadLocalStoreImpl::mergeInternal(PostMergeCb merge_complete_cb) {
  if (!shutting_down_) {
    for (const ParentHistogramSharedPtr& histogram : histograms()) {
      histogram->merge();
    }
    merge_complete_cb();
    merge_in_progress_ = false;
  }
}

void ThreadLocalStoreImpl::releaseScopeCrossThread(ScopeImpl* scope) {
  Thread::LockGuard lock(lock_);
  ASSERT(scopes_.count(scope) == 1);
  scopes_.erase(scope);

  // This can happen from any thread. We post() back to the main thread which will initiate the
  // cache flush operation.
  if (!shutting_down_ && main_thread_dispatcher_) {
    main_thread_dispatcher_->post(
        [this, scope_id = scope->scope_id_]() -> void { clearScopeFromCaches(scope_id); });
  }
}

std::string ThreadLocalStoreImpl::getTagsForName(const std::string& name,
                                                 std::vector<Tag>& tags) const {
  return tag_producer_->produceTags(name, tags);
}

void ThreadLocalStoreImpl::clearScopeFromCaches(uint64_t scope_id) {
  // If we are shutting down we no longer perform cache flushes as workers may be shutting down
  // at the same time.
  if (!shutting_down_) {
    // Perform a cache flush on all threads.
    tls_->runOnAllThreads(
        [this, scope_id]() -> void { tls_->getTyped<TlsCache>().scope_cache_.erase(scope_id); });
  }
}

absl::string_view ThreadLocalStoreImpl::truncateStatNameIfNeeded(absl::string_view name) {
  // If the main allocator requires stat name truncation, warn and truncate, before
  // attempting to allocate.
  if (alloc_.requiresBoundedStatNameSize()) {
    const uint64_t max_length = stats_options_.maxNameLength();

    // Note that the heap-allocator does not truncate itself; we have to
    // truncate here if we are using heap-allocation as a fallback due to an
    // exhausted shared-memory block
    if (name.size() > max_length) {
      ENVOY_LOG_MISC(
          warn,
          "Statistic '{}' is too long with {} characters, it will be truncated to {} characters",
          name, name.size(), max_length);
      name = absl::string_view(name.data(), max_length);
    }
  }
  return name;
}

std::atomic<uint64_t> ThreadLocalStoreImpl::ScopeImpl::next_scope_id_;

ThreadLocalStoreImpl::ScopeImpl::~ScopeImpl() { parent_.releaseScopeCrossThread(this); }

template <class StatType>
StatType& ThreadLocalStoreImpl::ScopeImpl::safeMakeStat(
    const std::string& name,
    StatMap<StatType>& central_cache_map,
    MakeStatFn<StatType> make_stat,
    StatMap<StatType>* tls_cache) {
    //StatType* tls_ref) {

  // If we have a valid cache entry, return it.
  /*
  if (tls_ref && *tls_ref) {
    return *tls_ref;
  }
  */
  //SymbolTable& table = *parent_.heap_allocator_.symbolTable();
  StatNameRef stat_name(name);
  //StatNameRefPtr stat_name = table.encode(name);
  if (tls_cache) {
    auto pos = tls_cache->find(stat_name);
    if (pos != tls_cache->end()) {
      //stat_name.free(table);
      return pos->second;
    }
  }

  // We must now look in the central store so we must be locked. We grab a reference to the
  // central store location. It might contain nothing. In this case, we allocate a new stat.
  Thread::LockGuard lock(parent_.lock_);
  //StatType& central_ref = central_cache_map[std::move(stat_name_ptr)];
  StatType* central_stat_ptr = nullptr;
  auto pos = central_cache_map.find(stat_name);
  if (pos != central_cache_map.end()) {
    central_stat_ptr = &pos->second;
  } else {
    std::vector<Tag> tags;

    // Tag extraction occurs on the original, untruncated name so the extraction
    // can complete properly, even if the tag values are partially truncated.
    std::string tag_extracted_name = parent_.getTagsForName(name, tags);
    absl::string_view truncated_name = parent_.truncateStatNameIfNeeded(name);
    StatType stat =
        make_stat(parent_.alloc_, truncated_name, std::move(tag_extracted_name), std::move(tags));
    if (stat == nullptr) {
      parent_.num_last_resort_stats_.inc();
      stat = make_stat(parent_.heap_allocator_, truncated_name, std::move(tag_extracted_name),
                       std::move(tags));
      ASSERT(stat != nullptr);
    }
    auto& central_ref = central_cache_map[stat->nameRef()];
    central_ref = stat;
    central_stat_ptr = &central_ref;
  }
  //stat_name.free(table);

  // If we have a TLS location to store or allocation into, do it.
  //if (tls_ref) {
  //  *tls_ref = central_ref;
  //}
  if (tls_cache) {
    tls_cache->insert(std::make_pair((*central_stat_ptr)->nameRef(), *central_stat_ptr));
  }

  // Finally we return the reference.
  return *central_stat_ptr;
}

Counter& ThreadLocalStoreImpl::ScopeImpl::counter(const std::string& name) {
  // Determine the final name based on the prefix and the passed name.
  std::string final_name = prefix_ + name;

  // We now try to acquire a *reference* to the TLS cache shared pointer. This might remain null
  // if we don't have TLS initialized currently. The de-referenced pointer might be null if there
  // is no cache entry.
  //CounterSharedPtr* tls_ref = nullptr;
  StatMap<CounterSharedPtr>* tls_cache = nullptr;
#if ENABLE_TLS_CACHE
  Thread::ReleasableLockGuard lock(parent_.lock_);
  //StatNamePtr stat_name_ptr = parent_.heap_allocator_.encode(final_name);
  if (!parent_.shutting_down_ && parent_.tls_) {
    tls_cache = &parent_.tls_->getTyped<TlsCache>().scopeCache(this->scope_id_).counters_;
  }
  lock.release();
#endif

  return *safeMakeStat<CounterSharedPtr>(
      final_name, central_cache_.counters_,
      [](StatDataAllocator& allocator, absl::string_view name, std::string&& tag_extracted_name,
         std::vector<Tag>&& tags) -> CounterSharedPtr {
        return allocator.makeCounter(name, std::move(tag_extracted_name), std::move(tags));
      },
      //tls_ref);
      tls_cache);
}

void ThreadLocalStoreImpl::ScopeImpl::deliverHistogramToSinks(const Histogram& histogram,
                                                              uint64_t value) {
  // Thread local deliveries must be blocked outright for histograms and timers during shutdown.
  // This is because the sinks may end up trying to create new connections via the thread local
  // cluster manager which may already be destroyed (there is no way to sequence this because the
  // cluster manager destroying can create deliveries). We special case this explicitly to avoid
  // having to implement a shutdown() method (or similar) on every TLS object.
  if (parent_.shutting_down_) {
    return;
  }

  for (Sink& sink : parent_.timer_sinks_) {
    sink.onHistogramComplete(histogram, value);
  }
}

Gauge& ThreadLocalStoreImpl::ScopeImpl::gauge(const std::string& name) {
  // See comments in counter(). There is no super clean way (via templates or otherwise) to
  // share this code so I'm leaving it largely duplicated for now.
  std::string final_name = prefix_ + name;
  //GaugeSharedPtr* tls_ref = nullptr;
  StatMap<GaugeSharedPtr>* tls_cache = nullptr;
#if ENABLE_TLS_CACHE
  Thread::ReleasableLockGuard lock(parent_.lock_);
  //StatNamePtr stat_name_ptr = parent_.heap_allocator_.encode(final_name);
  if (!parent_.shutting_down_ && parent_.tls_) {
    tls_cache = &parent_.tls_->getTyped<TlsCache>().scopeCache(this->scope_id_).gauges_;
  }
  lock.release();
#endif

  return *safeMakeStat<GaugeSharedPtr>(
      final_name, central_cache_.gauges_,
      [](StatDataAllocator& allocator, absl::string_view name, std::string&& tag_extracted_name,
         std::vector<Tag>&& tags) -> GaugeSharedPtr {
        return allocator.makeGauge(name, std::move(tag_extracted_name), std::move(tags));
      },
      //tls_ref);
      tls_cache);
}

Histogram& ThreadLocalStoreImpl::ScopeImpl::histogram(const std::string& name) {
  // See comments in counter(). There is no super clean way (via templates or otherwise) to
  // share this code so I'm leaving it largely duplicated for now.
  std::string final_name = prefix_ + name;
  ParentHistogramSharedPtr* tls_ref = nullptr;
  /*  SymbolTable* symbol_table;
  {
    Thread::LockGuard lock(parent_.lock_);
    symbol_table = parent_.heap_allocator_.symbolTable();
    }
    StatName stat_name = symbol_table->encode(final_name);*/
#if ENABLE_TLS_CACHE
  if (!parent_.shutting_down_ && parent_.tls_) {
    tls_ref = &parent_.tls_->getTyped<TlsCache>()
              .scopeCache(this->scope_id_)
              .parent_histograms_[name];
    if (*tls_ref) {
      //stat_name.free(symbol_table);
      return **tls_ref;
    }
    }
#endif

  ParentHistogramImplSharedPtr& central_ref =
      central_cache_.histograms_[name];
  if (!central_ref) {
    std::vector<Tag> tags;
    std::string tag_extracted_name = parent_.getTagsForName(final_name, tags);
    central_ref.reset(new ParentHistogramImpl(final_name, parent_, *this,
                                              std::move(tag_extracted_name), std::move(tags),
                                              parent_.symbol_table_));
  }

  if (tls_ref) {
    *tls_ref = central_ref;
  }
  //stat_name.free(symbol_table);
  return *central_ref;
}

Histogram& ThreadLocalStoreImpl::ScopeImpl::tlsHistogram(const std::string& name,
                                                         ParentHistogramImpl& parent) {
  // See comments in counter() which explains the logic here.

  // Here prefix will not be considered because, by the time ParentHistogram calls this method
  // during recordValue, the prefix is already attached to the name.
  TlsHistogramSharedPtr* tls_ref = nullptr;
  //Thread::LockGuard lock(parent_.lock_);
  //StatName stat_name = parent_.heap_allocator_.encode(name);
#if ENABLE_TLS_CACHE
  if (!parent_.shutting_down_ && parent_.tls_) {
    tls_ref = &parent_.tls_->getTyped<TlsCache>()
              .scopeCache(this->scope_id_)
              .histograms_[name];
  }

  if (tls_ref && *tls_ref) {
    return **tls_ref;
    }
#endif

  std::vector<Tag> tags;
  std::string tag_extracted_name = parent_.getTagsForName(name, tags);
  TlsHistogramSharedPtr hist_tls_ptr = std::make_shared<ThreadLocalHistogramImpl>(
      name, std::move(tag_extracted_name), std::move(tags), parent_.symbol_table_);

  parent.addTlsHistogram(hist_tls_ptr);

  if (tls_ref) {
    *tls_ref = hist_tls_ptr;
  }
  return *hist_tls_ptr;
}

ThreadLocalHistogramImpl::ThreadLocalHistogramImpl(const std::string& name,
                                                   std::string&& tag_extracted_name,
                                                   std::vector<Tag>&& tags,
                                                   SymbolTable& symbol_table)
    : MetricImpl(std::move(tag_extracted_name), std::move(tags), symbol_table),
      current_active_(0), flags_(0), created_thread_id_(std::this_thread::get_id()), name_(name) {
  histograms_[0] = hist_alloc();
  histograms_[1] = hist_alloc();
}

ThreadLocalHistogramImpl::~ThreadLocalHistogramImpl() {
  hist_free(histograms_[0]);
  hist_free(histograms_[1]);
}

void ThreadLocalHistogramImpl::recordValue(uint64_t value) {
  ASSERT(std::this_thread::get_id() == created_thread_id_);
  hist_insert_intscale(histograms_[current_active_], value, 0, 1);
  flags_ |= Flags::Used;
}

void ThreadLocalHistogramImpl::merge(histogram_t* target) {
  histogram_t** other_histogram = &histograms_[otherHistogramIndex()];
  hist_accumulate(target, other_histogram, 1);
  hist_clear(*other_histogram);
}

ParentHistogramImpl::ParentHistogramImpl(const std::string& name, Store& parent,
                                         TlsScope& tls_scope, std::string&& tag_extracted_name,
                                         std::vector<Tag>&& tags, SymbolTable& symbol_table)
    : MetricImpl(std::move(tag_extracted_name), std::move(tags), symbol_table), parent_(parent),
      tls_scope_(tls_scope), interval_histogram_(hist_alloc()), cumulative_histogram_(hist_alloc()),
      interval_statistics_(interval_histogram_), cumulative_statistics_(cumulative_histogram_),
      merged_(false), name_(name) {}

ParentHistogramImpl::~ParentHistogramImpl() {
  hist_free(interval_histogram_);
  hist_free(cumulative_histogram_);
}

void ParentHistogramImpl::recordValue(uint64_t value) {
  Histogram& tls_histogram = tls_scope_.tlsHistogram(name(), *this);
  tls_histogram.recordValue(value);
  parent_.deliverHistogramToSinks(*this, value);
}

bool ParentHistogramImpl::used() const {
  // Consider ParentHistogram used only if has ever been merged.
  return merged_;
}

void ParentHistogramImpl::merge() {
  Thread::ReleasableLockGuard lock(merge_lock_);
  if (merged_ || usedLockHeld()) {
    hist_clear(interval_histogram_);
    // Here we could copy all the pointers to TLS histograms in the tls_histogram_ list,
    // then release the lock before we do the actual merge. However it is not a big deal
    // because the tls_histogram merge is not that expensive as it is a single histogram
    // merge and adding TLS histograms is rare.
    for (const TlsHistogramSharedPtr& tls_histogram : tls_histograms_) {
      tls_histogram->merge(interval_histogram_);
    }
    // Since TLS merge is done, we can release the lock here.
    lock.release();
    hist_accumulate(cumulative_histogram_, &interval_histogram_, 1);
    cumulative_statistics_.refresh(cumulative_histogram_);
    interval_statistics_.refresh(interval_histogram_);
    merged_ = true;
  }
}

const std::string ParentHistogramImpl::summary() const {
  if (used()) {
    std::vector<std::string> summary;
    const std::vector<double>& supported_quantiles_ref = interval_statistics_.supportedQuantiles();
    summary.reserve(supported_quantiles_ref.size());
    for (size_t i = 0; i < supported_quantiles_ref.size(); ++i) {
      summary.push_back(fmt::format("P{}({},{})", 100 * supported_quantiles_ref[i],
                                    interval_statistics_.computedQuantiles()[i],
                                    cumulative_statistics_.computedQuantiles()[i]));
    }
    return absl::StrJoin(summary, " ");
  } else {
    return std::string("No recorded values");
  }
}

void ParentHistogramImpl::addTlsHistogram(const TlsHistogramSharedPtr& hist_ptr) {
  Thread::LockGuard lock(merge_lock_);
  tls_histograms_.emplace_back(hist_ptr);
}

bool ParentHistogramImpl::usedLockHeld() const {
  for (const TlsHistogramSharedPtr& tls_histogram : tls_histograms_) {
    if (tls_histogram->used()) {
      return true;
    }
  }
  return false;
}

} // namespace Stats
} // namespace Envoy
