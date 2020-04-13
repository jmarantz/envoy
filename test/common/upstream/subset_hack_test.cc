#include "gtest/gtest.h"

#include <iostream>
#include <random>

#include "common/common/base64.h"
#include "common/common/utility.h"
#include "common/upstream/subset_hack.h"

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_cat.h"

#include <iostream>

#define SWEEP_OVER_PERCENT true
#define ITERS 2000

namespace Envoy {
namespace Upstream {
namespace {

const uint64_t kuint64max = 0xffffffffffffffff;

TEST(SubsetHackTest, Disabled) {
  EXPECT_FALSE(SubsetHack::enabled());
}

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

TEST(SubsetHackTest, Allow) {
  //const std::vector<double> allows{0, .1, .15, .25, .5, .6, .75, .9, 1.0};
  //const std::vector<double> allows{.25};
  const uint32_t num_backends = 2000;
  const uint32_t num_envoys = 200;

#define SPEW_NAMES 0
#if SPEW_NAMES
  auto hex = [](uint64_t x) -> std::string {
               char buf[100];
               sprintf(buf, "%lx", x);
               return std::string(buf);
             };
#endif

  std::cout <<
      "% Subset,"
      "Mean Backends per Envoy,"
      "Min Backends,"
      "Max Backends,"
      "Backends - 1 sigma,"
      "Backends + 1 sigma,"
      "% Subset,"
      "Mean Load,"
      "Min Load,"
      "Max Load,"
      "Load - 1 sigma,"
      "Load + 1 sigma,"
            << std::endl;

  std::seed_seq seq{1,2,3};
  //absl::BitGen bit_gen(seq);

#define RANDOM_NAMES 1
#if RANDOM_NAMES
  //absl::InsecureBitGen bit_gen;
  //absl::uniform_int_distribution<uint64_t> dist(0, kuint64max);
  std::uniform_int_distribution<uint64_t> dist(0, kuint64max);
  std::mt19937_64 bit_gen;

  auto generate_name = [&dist, &bit_gen](absl::string_view prefix) -> std::string {
    uint64_t rand = dist(bit_gen);
    //uint64_t rand = twister();
    char* bytes = reinterpret_cast<char*>(reinterpret_cast<void*>(&rand));
    std::string base64 = Envoy::Base64::encode(bytes, sizeof(rand), false);
    return absl::StrCat(prefix, base64);
  };
#else
  uint64_t index = 0;
  auto generate_name = [&index](absl::string_view prefix) -> std::string {
    return absl::StrCat(prefix, ++index);
  };
#endif

  // RFC-1918 space is limited, so within the context of an iteration we need
  // to avoid duplicates by keeping a set.
  absl::flat_hash_set<std::string> ip_set;
  auto gen_rfc1918_ip = [&bit_gen, &ip_set]() -> std::string {
    std::uniform_int_distribution<uint8_t> dist(1, 255);
    std::string suffix;
    do {
      uint8_t a = dist(bit_gen);
      uint8_t b = dist(bit_gen);
      suffix = absl::StrCat(a, ".", b);
    } while (!ip_set.insert(suffix).second);
    return absl::StrCat("192.168.", suffix);
  };

#if SWEEP_OVER_PERCENT
  for (double allow = 0.05; allow <= 1.0; allow += 0.025)
#else
  for (double allow = 0.250; allow <= 0.25; allow += 0.025)
#endif
    {
    std::vector<double> backends_per_envoy_vector, /*envoys_*/ load_per_backend_vector;

    for (uint32_t iter = 0; iter < ITERS; ++iter) {
      std::vector<std::string> envoys;
      std::vector<std::string> backends;
      ip_set.clear();

      for (uint32_t i = 0; i < num_backends; ++i) {
        //backends.push_back(absl::StrCat("MyAwesomeBackend-", iter, "-", i));
        //backends.push_back(generate_name("backend-"));
        backends.push_back(gen_rfc1918_ip());
#if SPEW_NAMES
        std::cout << backends.back() << ":\t" << hex(HashUtil::xxHash64(backends.back())) << "\t"
                  << hex(absl::Hash<std::string>()(backends.back())) << "\n";
#endif
      }

      for (uint32_t i = 0; i < num_envoys; ++i) {
        //envoys.push_back(absl::StrCat("MyBeautifulEnvoy-", iter, "-", i));
        envoys.push_back(generate_name("envoy-"));
#if SPEW_NAMES
        std::cout << envoys.back() << ":\t" << HashUtil::xxHash64(envoys.back()) << "\t"
                  << absl::Hash<absl::string_view>()(envoys.back()) << "\n";
#endif
      }


      //const uint32_t allowable_backend_error = num_backends * allow / 2.0;
      //const uint32_t allowable_envoy_error = num_envoys * allow / 2.0;
      //uint32_t expected_allowed_backends = allow * num_backends;
      absl::flat_hash_map<std::string, uint32_t> envoys_per_backend;

      // Let's assume each Envoy gets 1k QPS. We can compute the load on each
      // backend.
      absl::flat_hash_map<std::string, double> load_per_backend;

      for (auto& envoy : envoys) {
        SubsetHack::enableSubsetting(envoy, allow);
        std::vector<std::string> backends_for_this_envoy;
        for (auto& backend : backends) {
          if (!SubsetHack::skipHost(backend)) {
            ++envoys_per_backend[backend];
            backends_for_this_envoy.push_back(backend);
            //envoy_to_backend_map[envoy].push_back(backend);
          }
        }

        if (!backends_for_this_envoy.empty()) {
          double qps_per_backend = 1000.0 / backends_for_this_envoy.size();
          for (const auto& backend : backends_for_this_envoy) {
            load_per_backend[backend] += qps_per_backend;
          }
        }

        //EXPECT_NEAR(expected_allowed_backends, count, allowable_backend_error)
        //    << "allow=" << allow << ", envoy=" << envoy;

#define EMIT_PER_BACKEND_CSV 0
#if EMIT_PER_BACKEND_CSV
        std::cout << envoy << "," << backends_for_this_envoy.size() << std::endl;
#endif
        backends_per_envoy_vector.push_back(backends_for_this_envoy.size());
      }

#if EMIT_PER_BACKEND_CSV
      std::cout << std::endl;
#endif

      //uint32_t expected_allowed_envoys = allow * num_envoys;
      for (auto& backend : backends) {
        //EXPECT_NEAR(expected_allowed_envoys, envoys_per_backend[backend], allowable_envoy_error)
        //    << "allow=" << allow << ", backend=" << backend;
#if EMIT_PER_BACKEND_CSV
        std::cout << backend << "," << envoys_per_backend[backend] << std::endl;
#endif
        //envoys_per_backend_vector.push_back(
        //    static_cast<double>(envoys_per_backend[backend]) / expected_allowed_envoys);
        load_per_backend_vector.push_back(load_per_backend[backend]);
      }
#if EMIT_PER_BACKEND_CSV
      std::cout << std::flush;
#endif
    }

    auto stat_csv = [allow](const Statistics& stats) {
                      std::cout << 100*allow << ","
                                << stats.mean() << ","
                                << stats.lo() << ","
                                << stats.hi() << ","
                                << stats.mean() - stats.stddev() << ","
                                << stats.mean() + stats.stddev();
                    };

    Statistics backends_per_envoy_stats(backends_per_envoy_vector);
    Statistics load_per_backend_stats(load_per_backend_vector);

    stat_csv(backends_per_envoy_stats);
    std::cout << ",";
    stat_csv(load_per_backend_stats);
    std::cout << std::endl;
  }
}

#if 0
TEST(SubsetHackTest, OldAllow) {
  //const std::vector<double> allows{0, .1, .15, .25, .5, .6, .75, .9, 1.0};
  const std::vector<double> allows{.25};
  std::vector<std::string> envoys;
  std::vector<std::string> backends;
  const uint32_t num_backends = 100;
  const uint32_t num_envoys = 200;

  for (uint32_t i = 0; i < num_backends; ++i) {
    backends.push_back(absl::StrCat("backend", i));
  }

  for (uint32_t i = 0; i < num_envoys; ++i) {
    envoys.push_back(absl::StrCat("envoy", i));
  }

  for (double allow : allows) {
    const uint32_t allowable_backend_error = num_backends * allow / 2.0;
    const uint32_t allowable_envoy_error = num_envoys * allow / 2.0;
    uint32_t expected_allowed_backends = allow * num_backends;
    absl::flat_hash_map<std::string, uint32_t> envoys_per_backend;
    for (auto& envoy : envoys) {
      SubsetHack::enableSubsetting(envoy, allow);
      uint32_t count = 0;
      for (auto& backend : backends) {
        if (!SubsetHack::skipHost(backend)) {
          ++count;
          ++envoys_per_backend[backend];
        }
      }
      EXPECT_NEAR(expected_allowed_backends, count, allowable_backend_error)
          << "allow=" << allow << ", envoy=" << envoy;

      std::cout << envoy << "," << count << std::endl;
    }

    std::cout << std::endl;

    uint32_t expected_allowed_envoys = allow * num_envoys;
    for (auto& backend : backends) {
      EXPECT_NEAR(expected_allowed_envoys, envoys_per_backend[backend], allowable_envoy_error)
          << "allow=" << allow << ", backend=" << backend;
      std::cout << backend << "," << envoys_per_backend[backend] << std::endl;
    }
    std::cout << std::flush;
  }
}
#endif

} // namespace
} // namespace Upstream
} // namespace Envoy
