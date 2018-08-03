#pragma once

#include "envoy/config/filter/http/cache/v2/cache.pb.h"

#include "extensions/filters/http/common/factory_base.h"
#include "extensions/filters/http/well_known_names.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Cache {

/**
 * Config registration for the cache filter. @see NamedHttpFilterConfigFactory.
 */
class CacheFilterFactory
    : public Common::FactoryBase<envoy::config::filter::http::cache::v2::Cache> {
public:
  CacheFilterFactory() : FactoryBase(HttpFilterNames::get().EnvoyCache) {}

private:
  Http::FilterFactoryCb
  createFilterFactoryFromProtoTyped(const envoy::config::filter::http::cache::v2::Cache& config,
                                    const std::string& stats_prefix,
                                    Server::Configuration::FactoryContext& context) override;
};

} // namespace Cache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
