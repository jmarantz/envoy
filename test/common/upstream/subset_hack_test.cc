#include <deque>
#include <iostream>
#include <random>

#include "common/common/base64.h"
#include "common/common/logger.h"
#include "common/common/utility.h"
#include "common/common/thread.h"
#include "common/upstream/subset_hack.h"

#include "test/test_common/environment.h"
#include "test/test_common/thread_factory_for_test.h"

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_cat.h"

#include <iostream>

#define SWEEP_OVER_PERCENT false
#define SWEEP_OVER_PRIMES false
#define SWEEP_OVER_BACKENDS true
#define DETAILED_CSV false

enum class LbPolicy { RoundRobin, LeastRequest, RequestOrdered };

constexpr double kAllow = 0.25;
#if !SWEEP_OVER_BACKENDS
constexpr uint32_t kNumBackends = 500;
#endif
constexpr uint32_t kNumEnvoys = 50;
constexpr uint32_t kIters = 100;
constexpr uint32_t kSimCycles = 100;
constexpr uint32_t kXorBits = 8;
constexpr uint32_t kPrime = 21397;

namespace Envoy {
namespace Upstream {
namespace {

const uint64_t kuint64max = 0xffffffffffffffff;

class Statistics {
 public:
  Statistics(const std::vector<double>& data) {
    if (data.empty()) {
      return;
    }
    WelfordStandardDeviation stddev;
    double total = 0;
    lo_ = hi_ = data[0];
    for (double value : data) {
      stddev.update(value);
      total += value;
      lo_ = std::min(lo_, value);
      hi_ = std::max(hi_, value);
    }
    mean_ = total / data.size();
    stddev_ = stddev.computeStandardDeviation();
  }

  double mean() const { return mean_; }
  double lo() const { return lo_; }
  double hi() const { return hi_; }
  double stddev() const { return stddev_; }

 private:
  double mean_{0};
  double lo_{0};
  double hi_{0};
  double stddev_{0};
};

class LoadBalancer {
 public:
  virtual ~LoadBalancer() = default;

  virtual uint32_t pickBackend() PURE;
  virtual void retireRequest(uint32_t subset_index) PURE;
};

class RoundRobinBalancer : public LoadBalancer {
 public:
  RoundRobinBalancer(uint32_t subset_size, std::mt19937_64& bit_gen)
      : subset_size_(subset_size) {
    std::uniform_int_distribution<uint32_t> dist{0, subset_size_ - 1};
    round_robin_index_ = dist(bit_gen);
  }

  uint32_t pickBackend() override {
    round_robin_index_ = (round_robin_index_ + 1) % subset_size_;
    return round_robin_index_;
  }

  void retireRequest(uint32_t /*subset_index*/) override {}

 private:
  uint32_t subset_size_;
  uint32_t round_robin_index_;
};

class LeastRequestBalancer : public LoadBalancer {
 public:
  LeastRequestBalancer(uint32_t subset_size, std::mt19937_64& bit_gen)
      : bit_gen_(bit_gen) {
    outstanding_requests_.resize(subset_size);
  }

  uint32_t pickBackend() override {
    uint32_t max_subset_index = outstanding_requests_.size() - 1;
    std::uniform_int_distribution<uint32_t> dist{0, max_subset_index};
    uint32_t i = dist(bit_gen_), j;
    do {
      j = dist(bit_gen_);
    } while (i == j);

    uint32_t subset_index = (outstanding_requests_[i] <= outstanding_requests_[j]) ? i : j;
    ++outstanding_requests_[subset_index];
    return subset_index;
  }

  void retireRequest(uint32_t subset_index) override {
    ASSERT(outstanding_requests_.size() > subset_index);
    ASSERT(outstanding_requests_[subset_index] > 0);
    --outstanding_requests_[subset_index];
  }

 private:
  std::mt19937_64& bit_gen_;
  std::vector<uint32_t> outstanding_requests_;
};

class RequestOrderedBalancer : public LoadBalancer {
 public:
  RequestOrderedBalancer(std::vector<uint32_t>& subset, std::mt19937_64& bit_gen)
      : request_order_(BackendCompare(*this)) {
    uint32_t subset_size = subset.size();

    // Shuffle the subset to reduce the frequency that different Envoys pick
    // the same backends in the same order.
    for (uint32_t i = subset_size - 1; i > 0; --i) {
      std::uniform_int_distribution<uint32_t> dist(0, i);
      std::swap(subset[i], subset[dist(bit_gen)]);
    }

    // Populate the std::set and initialize the outstanding_request_ array.
    outstanding_requests_.reserve(subset_size);
    for (uint32_t i = 0; i < subset_size; ++i) {
      outstanding_requests_.push_back(0);
      request_order_.insert(i);
    }
  }

  uint32_t pickBackend() override {
    auto iter = request_order_.begin();
    uint32_t subset_index = *iter;
    request_order_.erase(iter);
    ++outstanding_requests_[subset_index];
    request_order_.insert(subset_index);
    return subset_index;
  }

  void retireRequest(uint32_t subset_index) override {
    request_order_.erase(subset_index);
    --outstanding_requests_[subset_index];
    request_order_.insert(subset_index);
  }

 private:
  struct BackendCompare {
    explicit BackendCompare(RequestOrderedBalancer& balancer) : balancer_(balancer) {}

    bool operator()(uint32_t a, uint32_t b) const {
      uint32_t a_requests = balancer_.outstanding_requests_[a];
      uint32_t b_requests = balancer_.outstanding_requests_[b];
      if (a_requests < b_requests) {
        return true;
      } else if (a_requests > b_requests) {
        return false;
      }
      return a < b;
    }

    RequestOrderedBalancer& balancer_;
  };


  std::vector<uint32_t> outstanding_requests_;
  std::set<uint32_t, BackendCompare> request_order_;
};

class Sweep {
 public:
  Sweep(SubsetHack::Strategy strategy, SubsetHack::HashChoice hasher, LbPolicy lb_policy,
        std::function<void()> progress)
      : strategy_(strategy), hasher_(hasher), lb_policy_(lb_policy), progress_(progress) {}

  struct EnvoyInstance;

  struct Request {
    uint32_t retire_at_cycle_;
    uint32_t envoy_index_;
    uint32_t subset_index_;  // index into Envoy's backend_subset_array_.
  };

  struct BackendInstance {
    explicit BackendInstance(const std::string& name) : name_(name) {}
    std::string name_;
    //double load_{0};
    uint32_t max_queue_size_{0};
    uint32_t max_latency_{0};
    std::deque<Request> requests_;
    std::vector<uint32_t> envoys_;  // Index into envoy_instances_.
  };

  // Each Envoy only knows about the request that *it* has outstanding to
  // a particular backend, as we have no protocol for the backend to
  // backpressure an Envoy. So only when this particular Envoy sends
  // a second outstanding request to the backend will it be useful for
  // backoff via least_request.
  struct BackendAsSeenByEnvoy {
    uint32_t index_;                // into backend_instances_;
    uint32_t outstanding_requests_; // from this envoy.
  };

  struct EnvoyInstance {
    explicit EnvoyInstance(const std::string& name) : name_(name) {}
    std::string name_;
    //std::vector<BackendAsSeenByEnvoy> backend_subset_;
    std::vector<uint32_t> backend_subset_;
    std::unique_ptr<LoadBalancer> load_balancer_;
  };

  std::string describe() {
    std::string out;
    switch (strategy_) {
      case SubsetHack::Strategy::Modulus: out = "mod"; break;
      case SubsetHack::Strategy::Xor: out = "xor"; break;
      case SubsetHack::Strategy::XorReverse: out = "xorR"; break;
      default: out = "other"; break;
    }

    switch (hasher_) {
      case SubsetHack::HashChoice::Absl: out += "/absl"; break;
      case SubsetHack::HashChoice::AbslCombine: out += "/absl-combine"; break;
      case SubsetHack::HashChoice::XX: out += "/xx"; break;
      case SubsetHack::HashChoice::Sha1: out += "/sha1"; break;
    }

    switch (lb_policy_) {
      case LbPolicy::RoundRobin: out += "/rr"; break;
      case LbPolicy::LeastRequest: out += "/lr"; break;
      case LbPolicy::RequestOrdered: out += "/ro"; break;
    }

    return out;
  }

  uint64_t totalOps() const {
    uint64_t total = kIters * kSimCycles;
#if SWEEP_OVER_BACKENDS
    uint64_t backend_options = 0;
    for (uint32_t num_backends = 40; num_backends <= 3000; num_backends *= 1.3) {
      ++backend_options;
    }
    total *= backend_options;
#endif
#if SWEEP_OVER_PERCENT
    uint64_t allow_options = 0;
    for (double allow = 0.05; allow <= 1.0; allow += 0.025) {
      ++allow_options;
    }
    total *= allow_options;
#endif
#if SWEEP_OVER_PRIMES
    total *= primes_.size();
#endif
    return total;
  }

  void run() {
#if SWEEP_OVER_BACKENDS
    for (uint32_t num_backends = 40; num_backends <= 3000; num_backends *= 1.3) {
#else
      const uint32_t num_backends = kNumBackends;
#endif

#if SWEEP_OVER_PERCENT
      for (double allow = 0.05; allow <= 1.0; allow += 0.025) {
#else
        const double allow = kAllow;
#endif

#if SWEEP_OVER_PRIMES
        for (uint32_t prime : primes_) {
#else
          const uint32_t prime = kPrime;
#endif
          clearTrialData();

          for (uint32_t iter = 0; iter < kIters; ++iter) {
            trial(prime, allow, num_backends);
          }

          emitResults(prime, allow, num_backends);
#if SWEEP_OVER_PRIMES
        }
#endif

#if SWEEP_OVER_BACKENDS
      }
#endif

#if SWEEP_OVER_PERCENT
    }
#endif
  }

  void clearTrialData() {
    backends_per_envoy_vector_.clear();
    load_per_backend_vector_.clear();
  }

  // Retires the next pending request for a backend, if it is ready.
  void retireRequest(uint32_t backend_index, uint32_t cycle) {
    BackendInstance& backend = *backend_instances_[backend_index];
    if (!backend.requests_.empty()) {
      Request& request = backend.requests_.front();
      if (request.retire_at_cycle_ <= cycle) {
        EnvoyInstance& envoy = *envoy_instances_[request.envoy_index_];
        envoy.load_balancer_->retireRequest(request.subset_index_);
        backend.requests_.pop_front();
      }
    }
  }

  /*
  uint32_t pickBackendRoundRobin(EnvoyInstance& envoy) {
    envoy.round_robin_index_ = (envoy.round_robin_index_ + 1) % envoy.backend_subset_.size();
    return envoy.round_robin_index_;
  }

  uint32_t pickBackendLeastRequest(EnvoyInstance& envoy) {
    uint32_t max_subset_index = envoy.backend_subset_.size() - 1;
    std::uniform_int_distribution<uint32_t> dist{0, max_subset_index};
    uint32_t i = dist(bit_gen_), j;
    while ((j = dist(bit_gen_)) == i) {
    }

    return (envoy.backend_subset_[i].outstanding_requests_ <=
            envoy.backend_subset_[j].outstanding_requests_) ? i : j;
  }
  */

  // Initiates a request from an Envoy to a backend, based on load-balancing
  // policy. For now we are picking least-loaded.
  void makeRequest(uint32_t envoy_index, uint32_t cycle) {
    // pick two random backends.
    EnvoyInstance& envoy = *envoy_instances_[envoy_index];
    uint32_t subset_index = envoy.load_balancer_->pickBackend();
    uint32_t backend_index = envoy.backend_subset_[subset_index];
    BackendInstance& backend = *backend_instances_[backend_index];
    uint32_t retire_at_cycle = cycle + cycles_to_retire_;
    uint32_t latency = cycles_to_retire_;
    if (!backend.requests_.empty()) {
      uint32_t new_retire_at_cycle = backend.requests_.back().retire_at_cycle_ +
                                     cycles_to_retire_;
      latency += new_retire_at_cycle - retire_at_cycle;
    }
    backend.max_latency_ = std::max(backend.max_latency_, latency);
    backend.requests_.push_back(Request{
        retire_at_cycle, envoy_index, subset_index});
    uint32_t num_requests = backend.requests_.size();
    backend.max_queue_size_ = std::max(backend.max_queue_size_, num_requests);
  }

  void trial(uint32_t prime, double allow, uint32_t num_backends) {
    ip_set_.clear();
    envoy_instances_.clear();
    backend_instances_.clear();
    cycles_to_retire_ = (4*num_backends + kNumEnvoys - 1) / kNumEnvoys;

    for (uint32_t i = 0; i < num_backends; ++i) {
      backend_instances_.push_back(std::make_unique<BackendInstance>(
          generateRFC1918Ip()));
    }

    for (uint32_t i = 0; i < kNumEnvoys; ++i) {
      envoy_instances_.push_back(std::make_unique<EnvoyInstance>(
          generateName("envoy-")));
    }

    // Assign all the envoys to backends.
    for (uint32_t e = 0; e < envoy_instances_.size(); ++e) {
      EnvoyInstance& envoy = *envoy_instances_[e];
      SubsetHack subset_hack(
          hasher_, strategy_, kXorBits, prime, envoy.name_, allow);
      std::vector<std::string> backends_for_this_envoy;
      for (uint32_t b = 0; b < backend_instances_.size(); ++b) {
        BackendInstance& backend = *backend_instances_[b];
        if (!subset_hack.skipHost(backend.name_)) {
          backend.envoys_.push_back(e);
          envoy.backend_subset_.push_back(b);
        }
      }
      uint32_t subset_size = envoy.backend_subset_.size();
      switch (lb_policy_) {
        case LbPolicy::RoundRobin:
          envoy.load_balancer_ = std::make_unique<RoundRobinBalancer>(subset_size, bit_gen_);
          break;
        case LbPolicy::LeastRequest:
          envoy.load_balancer_ = std::make_unique<LeastRequestBalancer>(subset_size, bit_gen_);
          break;
        case LbPolicy::RequestOrdered:
          envoy.load_balancer_ = std::make_unique<RequestOrderedBalancer>(
              envoy.backend_subset_, bit_gen_);
          break;
      }
    }

    // Let's assume each backend is single-threaded, and can process 100 qps.
    //
    // Determine the load per backend. We'll test a variety of scenarios, but
    // let's take an example with made up numbers.
    //
    // Let's assume:
    //    Each Envoy gets 1k QPS from clients.
    //    Each backend can process
    //        1k * (num_envoys / num_backends) * 1.3 QPS.
    //
    // For example, if there are 200 Envoys and 2000 backends, then each
    // backend can process 1k*(200/2000)*1.2 = 130 qps, which implies the
    // system is overprovisioned by 30%.
    //
    // With perfect 25% subsetting, we'd expect each Envoy to be assigned 500
    // backends, and each backend to be sent load from 50 of the 200 Envoys.
    // Due to imperfect balancing, though, some backends may be reachable by
    // (say) as many as 85 Envoys (1.7 peak/mean), and so they will fall
    // behind on their queries, queuing them an increasing latency. Worse, the
    // queue will continue to grow if queries continue come in faster than
    // they can be processed. There are two remedies:
    //
    //   - incrementally migrate the subset over time, to change which
    //     backends receive an imbalance, at the cost of controlled churn.
    //   - employ the least_request load-balancing policy at the Envoys,
    //     to shift queries away from backends that are starting to queue
    //     requests.
    for (uint32_t cycle = 0; cycle < kSimCycles; ++cycle) {
      progress_();

      // Retire any sufficiently aged requests.
      for (uint32_t b = 0; b < backend_instances_.size(); ++b) {
        retireRequest(b, cycle);
      }

      // Make a single request for each Envoy.
      for (uint32_t e = 0; e < envoy_instances_.size(); ++e) {
        makeRequest(e, cycle);
      }
    }

    /*
     *    if (!backends_for_this_envoy.empty()) {
     *      double qps_per_backend = 1000.0 / backends_for_this_envoy.size();
     *      for (const auto& backend : backends_for_this_envoy) {
     *        load_per_backend[backend] += qps_per_backend;
     *      }
     *    }
     *
     *    backends_per_envoy_vector_.push_back(backends_for_this_envoy.size());
     *
     *    for (auto& backend : backends) {
     *      load_per_backend_vector_.push_back(load_per_backend[backend]);
     *    }
     */


    for (uint32_t i = 0; i < backend_instances_.size(); ++i) {
      BackendInstance& backend = *backend_instances_[i];
      load_per_backend_vector_.push_back(backend.max_queue_size_);
    }
  }

  void writeCsvHeader() {
    output_ =
        "Strategy,"
        "% Subset,"
        "Prime,"
        "Total Backends,"
        "Peak/Mean Backends per Envoy,"
        "Mean Backends per Envoy,"
        "Min Backends,"
        "Max Backends,"
        "Backends - 1 sigma,"
        "Backends + 1 sigma,"
        "Total Backends,"
        "Peak/Mean Load,"
        "Mean Load,"
        "Min Load,"
        "Max Load,"
        "Load - 1 sigma,"
        "Load + 1 sigma\n";
  }

#if DETAILED_CSV
  void writeCsvLine(const Statistics& stats, uint32_t num_backends) {
    absl::StrAppend(&output_,
                    num_backends, ","
                    , stats.hi() / stats.mean(), ","
                    , stats.mean(), ","
                    , stats.lo(), ","
                    , stats.hi(), ","
                    , stats.mean() - stats.stddev(), ","
                    , stats.mean() + stats.stddev());
  }

  void flushDetailedCsv() {
    std::cout << output_ << "\n\n";
  }
#endif

  void emitResults(uint32_t prime, double allow, uint32_t num_backends) {
    Statistics backends_per_envoy_stats(backends_per_envoy_vector_);
    Statistics load_per_backend_stats(load_per_backend_vector_);

#if DETAILED_CSV
    absl::StrAppend(&output_, describe(), ",", 100*allow, ",", prime, ",");
    writeCsvLine(backends_per_envoy_stats, num_backends);
    absl::StrAppend(&output_, ",");
    writeCsvLine(load_per_backend_stats, num_backends);
    absl::StrAppend(&output_, "\n");
#else
    UNREFERENCED_PARAMETER(prime);
    double peak_to_mean = load_per_backend_stats.hi() /
                          load_per_backend_stats.mean();
    peak_.push_back(load_per_backend_stats.hi());
    mean_.push_back(load_per_backend_stats.mean());
    peak_to_mean_.push_back(peak_to_mean);
    plus_1_sigma_.push_back((load_per_backend_stats.mean() + load_per_backend_stats.stddev()) /
                            load_per_backend_stats.mean());
    num_backends_.push_back(num_backends);
    allow_.push_back(allow);
#endif
  }

  std::string generateName(absl::string_view prefix) {
    uint64_t rand = dist64_(bit_gen_);
    char* bytes = reinterpret_cast<char*>(reinterpret_cast<void*>(&rand));
    std::string base64 = Envoy::Base64::encode(bytes, sizeof(rand), false);
    return absl::StrCat(prefix, base64);
  }

  // RFC-1918 space is limited, so within the context of an iteration we need
  // to avoid duplicates by keeping a set.
  std::string generateRFC1918Ip() {
    std::string suffix;
    do {
      uint8_t a = dist8_(bit_gen_);
      uint8_t b = dist8_(bit_gen_);
      suffix = absl::StrCat(a, ".", b);
    } while (!ip_set_.insert(suffix).second);
    return absl::StrCat("192.168.", suffix);
  }

  const SubsetHack::Strategy strategy_;
  const SubsetHack::HashChoice hasher_;
  const LbPolicy lb_policy_;
  std::string output_;
  uint32_t cycles_to_retire_{0};
  std::vector<uint32_t> num_backends_;
  std::vector<double> allow_;
  std::vector<double> peak_;
  std::vector<double> mean_;
  std::vector<double> peak_to_mean_;
  std::vector<double> plus_1_sigma_;
  std::function<void()> progress_;
  std::uniform_int_distribution<uint64_t> dist64_{0, kuint64max};
  std::uniform_int_distribution<uint8_t> dist8_{1, 255};
  std::mt19937_64 bit_gen_;
  absl::flat_hash_set<std::string> ip_set_;
#if SWEEP_OVER_PRIMES
  const std::vector<uint32_t> primes_{
    2003, 2411, 2887, 3457, 4153, 4987, 5981, 7177, 8599,
    10321, 12391, 14867, 17837, 21397, 25673, 30809, 36973, 44357, 53231,
    63901, 76649, 91997
  };
#endif
  std::vector<double> backends_per_envoy_vector_;
  std::vector<double> load_per_backend_vector_;
  std::vector<std::unique_ptr<EnvoyInstance>> envoy_instances_;
  std::vector<std::unique_ptr<BackendInstance>> backend_instances_;
};

class TestContext {
 public:
  TestContext() : thread_factory_(Thread::threadFactoryForTest()) {}

  void run() {
    sweep(SubsetHack::Strategy::Modulus, SubsetHack::HashChoice::Absl, LbPolicy::RoundRobin);
    sweep(SubsetHack::Strategy::Modulus, SubsetHack::HashChoice::Absl, LbPolicy::LeastRequest);
    sweep(SubsetHack::Strategy::Modulus, SubsetHack::HashChoice::Absl, LbPolicy::RequestOrdered);
    //sweep(SubsetHack::Strategy::Modulus, SubsetHack::HashChoice::Absl);
    //sweep(SubsetHack::Strategy::Modulus, SubsetHack::HashChoice::XX);
    //sweep(SubsetHack::Strategy::Xor, SubsetHack::HashChoice::Absl);
    //sweep(SubsetHack::Strategy::Xor, SubsetHack::HashChoice::XX);
    //sweep(SubsetHack::Strategy::XorReverse, SubsetHack::HashChoice::Absl);
    //sweep(SubsetHack::Strategy::XorReverse, SubsetHack::HashChoice::XX);

    for (auto& thread : threads_) {
      thread->join();
    }

#if DETAILED_CSV
    for (auto& sweep : sweeps_) {
      sweep->flushDetailedCsv();
    }
#else
    std::cout << "num backends,subset%";
    for (auto& sweep : sweeps_) {
      std::cout << "," << sweep->describe() << " peak";
    }
    for (auto& sweep : sweeps_) {
      std::cout << "," << sweep->describe() << " mean";
    }
    for (auto& sweep : sweeps_) {
      std::cout << "," << sweep->describe() << " p/m";
    }
    for (auto& sweep : sweeps_) {
      std::cout << "," << sweep->describe() << " +1s";
    }
    std::cout << std::endl;
    for (uint32_t i = 0; i < sweeps_[0]->num_backends_.size(); ++i) {
      std::cout << sweeps_[0]->num_backends_[i] << "," << 100*sweeps_[0]->allow_[i];
      for (auto& sweep : sweeps_) {
        std::cout << "," << sweep->peak_[i];
      }
      for (auto& sweep : sweeps_) {
        std::cout << "," << sweep->mean_[i];
      }
      for (auto& sweep : sweeps_) {
        std::cout << "," << sweep->peak_to_mean_[i];
      }
      for (auto& sweep : sweeps_) {
        std::cout << "," << sweep->plus_1_sigma_[i];
      }
      std::cout << std::endl;
    }
#endif
  }

 private:
  void progress() {
    uint64_t ops = ++ops_so_far_;
    const uint64_t interval = 5000;
    if ((ops % interval) == 0) {
      std::cerr << "Progress: " << ops/interval << "/"
                << total_ops_/interval << std::endl;
    }
  }

  void sweep(SubsetHack::Strategy strategy, SubsetHack::HashChoice hasher,
             LbPolicy lb_policy) {
    sweeps_.push_back(std::make_unique<Sweep>(strategy, hasher, lb_policy,
                                              [this] { progress(); }));;
    Sweep& swp = *sweeps_.back();
    total_ops_ += swp.totalOps();
    threads_.push_back(thread_factory_.createThread([&swp]() { swp.run(); }));
  }

  Thread::ThreadFactory& thread_factory_;
  std::vector<Envoy::Thread::ThreadPtr> threads_;
  std::vector<std::unique_ptr<Sweep>> sweeps_;
  std::atomic<uint64_t> total_ops_{0};
  std::atomic<uint64_t> ops_so_far_{0};
};

} // namespace
} // namespace Upstream
} // namespace Envoy

int main(int argc, char** argv) {
  Envoy::TestEnvironment::initializeOptions(argc, argv);
  Envoy::Thread::MutexBasicLockable lock;
  auto& options = Envoy::TestEnvironment::getOptions();
  Envoy::Logger::Context logging_state(options.logLevel(), options.logFormat(), lock, false);
  Envoy::Upstream::TestContext test_context;
  test_context.run();
  return 0;
}
