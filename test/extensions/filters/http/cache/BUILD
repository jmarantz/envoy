licenses(["notice"])  # Apache 2

load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_test",
    "envoy_package",
)

envoy_package()

envoy_cc_test(
    name = "cache_filter_test",
    srcs = ["cache_filter_test.cc"],
    deps = [
        "//source/common/compressor:compressor_lib",
        "//source/common/decompressor:decompressor_lib",
        "//source/common/protobuf:utility_lib",
        "//source/extensions/filters/http/cache:cache_filter_lib",
        "//test/mocks/http:http_mocks",
        "//test/mocks/runtime:runtime_mocks",
        "//test/test_common:utility_lib",
    ],
)

envoy_cc_test(
    name = "cache_filter_integration_test",
    srcs = [
        "cache_filter_integration_test.cc",
    ],
    deps = [
        "//source/common/decompressor:decompressor_lib",
        "//source/extensions/filters/http/cache:config",
        "//test/integration:http_integration_lib",
        "//test/test_common:utility_lib",
    ],
)
