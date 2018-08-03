#include "extensions/filters/http/cache/config.h"

#include "envoy/config/filter/http/cache/v2/cache.pb.validate.h"
#include "envoy/registry/registry.h"

#include "extensions/filters/http/cache/cache_filter.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Cache {

Http::FilterFactoryCb CacheFilterFactory::createFilterFactoryFromProtoTyped(
    const envoy::config::filter::http::cache::v2::Cache& proto_config,
    const std::string& stats_prefix, Server::Configuration::FactoryContext& context) {
  CacheFilterConfigSharedPtr config = std::make_shared<CacheFilterConfig>(
      proto_config, stats_prefix, context.scope(), context.runtime());
  return [config](Http::FilterChainFactoryCallbacks& callbacks) -> void {
    callbacks.addStreamFilter(std::make_shared<CacheFilter>(config));
  };
}

/**
 * Static registration for the cache filter. @see NamedHttpFilterConfigFactory.
 */
static Registry::RegisterFactory<CacheFilterFactory,
                                 Server::Configuration::NamedHttpFilterConfigFactory>
    register_;

} // namespace Cache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
