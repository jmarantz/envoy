licenses(["notice"])  # Apache 2

load(
    "//bazel:envoy_build_system.bzl",
    "envoy_basic_cc_library",
    "envoy_cc_library",
    "envoy_include_prefix",
    "envoy_package",
)

envoy_package()

envoy_cc_library(
    name = "ecache_lib",
    hdrs = ["ecache.h"],
    deps = [
      "//include/envoy/common:time_interface",
      "//source/common/common:assert_lib",
      "//source/common/common:utility_lib",
    ],
)

envoy_cc_library(
    name = "simple_cache_lib",
    hdrs = ["simple_cache.h"],
    srcs = ["simple_cache.cc"],
    deps = [
      "ecache_lib",
      "//source/common/common:lock_guard_lib",
      "//source/common/common:thread_lib",
    ],
)

