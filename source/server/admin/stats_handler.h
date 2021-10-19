#pragma once

#include <regex>
#include <string>

#include "envoy/buffer/buffer.h"
#include "envoy/http/codes.h"
#include "envoy/http/header_map.h"
#include "envoy/server/admin.h"
#include "envoy/server/instance.h"

#include "source/common/stats/histogram_impl.h"
#include "source/server/admin/handler_ctx.h"

#include "absl/strings/string_view.h"

namespace Envoy {
namespace Server {

class StatsHandler : public HandlerContextBase {

public:
  enum class Type {
    Counter,
    Gauge,
    Histogram,
    TextResult,
  };

  StatsHandler(Server::Instance& server);

  Http::Code handlerResetCounters(absl::string_view path_and_query,
                                  Http::ResponseHeaderMap& response_headers,
                                  Buffer::Instance& response, AdminStream&);
  Http::Code handlerStatsRecentLookups(absl::string_view path_and_query,
                                       Http::ResponseHeaderMap& response_headers,
                                       Buffer::Instance& response, AdminStream&);
  Http::Code handlerStatsRecentLookupsClear(absl::string_view path_and_query,
                                            Http::ResponseHeaderMap& response_headers,
                                            Buffer::Instance& response, AdminStream&);
  Http::Code handlerStatsRecentLookupsDisable(absl::string_view path_and_query,
                                              Http::ResponseHeaderMap& response_headers,
                                              Buffer::Instance& response, AdminStream&);
  Http::Code handlerStatsRecentLookupsEnable(absl::string_view path_and_query,
                                             Http::ResponseHeaderMap& response_headers,
                                             Buffer::Instance& response, AdminStream&);
  Http::Code handlerStats(absl::string_view path_and_query,
                          Http::ResponseHeaderMap& response_headers, Buffer::Instance& response,
                          AdminStream&);
  Http::Code handlerPrometheusStats(absl::string_view path_and_query,
                                    Http::ResponseHeaderMap& response_headers,
                                    Buffer::Instance& response, AdminStream&);
  Http::Code handlerHtmlStats(absl::string_view path_and_query,
                              Http::ResponseHeaderMap& response_headers, Buffer::Instance& response,
                              AdminStream&, Stats::StatName after, Type type);
  Http::Code handlerStatsScopes(absl::string_view path_and_query,
                                Http::ResponseHeaderMap& response_headers,
                                Buffer::Instance& response, AdminStream&);
  Http::Code handlerContention(absl::string_view path_and_query,
                               Http::ResponseHeaderMap& response_headers,
                               Buffer::Instance& response, AdminStream&);

private:
  enum class Format {
    Text,
    Json,
    Prometheus,
    // Html,
  };

  struct Params {
    Http::Code parse(absl::string_view url, Buffer::Instance& response);
    template <class StatType> bool shouldShowMetric(const StatType& metric) const {
      return ((!used_only_ || metric.used()) &&
              (!filter_.has_value() || std::regex_search(metric.name(), filter_.value())));
    }

    bool used_only_{false};
    bool pretty_{false};
    Format format_{Format::Text};
    absl::optional<std::regex> filter_;
    absl::optional<std::string> scope_;
  };

  friend class AdminStatsTest;

  static Http::Code stats(const Params& parmams, Stats::Store& stats,
                          Http::ResponseHeaderMap& response_headers, Buffer::Instance& response);

  static Http::Code prometheusStats(absl::string_view path_and_query, Buffer::Instance& response,
                                    Stats::Store& stats,
                                    Stats::CustomStatNamespaces& custom_namespaces);

  static std::string statsAsJson(const std::map<std::string, uint64_t>& all_stats,
                                 const std::map<std::string, std::string>& text_readouts,
                                 const std::vector<Stats::HistogramSharedPtr>& all_histograms,
                                 bool pretty_print);

  static void statsAsText(const std::map<std::string, uint64_t>& all_stats,
                          const std::map<std::string, std::string>& text_readouts,
                          const std::vector<Stats::HistogramSharedPtr>& all_histograms,
                          Buffer::Instance& response);

  Http::Code statsAsHtml(const Http::Utility::QueryParams& query_params,
                         Http::ResponseHeaderMap& response_headers, Buffer::Instance& response,
                         AdminStream&, bool used_only, absl::optional<std::regex>& filter);
  template <class StatType>
  Http::Code renderHtml(Stats::StatName after, uint32_t page_size, bool used_only,
                        absl::optional<std::regex>& /*filter*/,
                        Http::ResponseHeaderMap& response_headers, Buffer::Instance& response);
};

} // namespace Server
} // namespace Envoy
