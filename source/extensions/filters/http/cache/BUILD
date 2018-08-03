licenses(["notice"])  # Apache 2

# HTTP L7 filter that performs cache compression
# Public docs: docs/root/configuration/http_filters/cache_filter.rst

load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_library",
    "envoy_package",
)

envoy_package()

envoy_cc_library(
    name = "cache_filter_lib",
    srcs = ["cache_filter.cc"],
    hdrs = ["cache_filter.h"],
    deps = [
        "//include/envoy/http:filter_interface",
        "//include/envoy/json:json_object_interface",
        "//include/envoy/runtime:runtime_interface",
        "//source/common/buffer:buffer_lib",
        "//source/common/compressor:compressor_lib",
        "//source/common/http:header_map_lib",
        "//source/common/http:headers_lib",
        "//source/common/json:config_schemas_lib",
        "//source/common/json:json_validator_lib",
        "//source/common/protobuf",
        "//source/extensions/cache:cache_lib",
        "//source/extensions/cache:simple_cache_lib",
        "@envoy_api//envoy/config/filter/http/cache/v2:cache_cc",
    ],
)

envoy_cc_library(
    name = "config",
    srcs = ["config.cc"],
    hdrs = ["config.h"],
    deps = [
        "//include/envoy/registry",
        "//source/extensions/filters/http:well_known_names",
        "//source/extensions/filters/http/cache:cache_filter_lib",
        "//source/extensions/filters/http/common:factory_base_lib",
    ],
)
