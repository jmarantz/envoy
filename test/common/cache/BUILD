licenses(["notice"])  # Apache 2

load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_binary",
    "envoy_cc_test",
    "envoy_package",
)

envoy_package()

envoy_cc_test(
    name = "simple_cache_test",
    srcs = ["simple_cache_test.cc"],
    deps = [
        "//source/common/cache:simple_cache_lib",
    ],
)
