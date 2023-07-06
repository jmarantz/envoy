#pragma once

#include "envoy/common/optref.h"
#include "envoy/server/admin.h"
#include "envoy/stats/stats.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/json/json_streamer.h"
#include "source/server/admin/stats_params.h"
#include "source/server/admin/utils.h"

namespace Envoy {
namespace Server {

// Abstract class for rendering stats. Every method is called "generate"
// differing only by the data type, to facilitate templatized call-sites.
//
// There are currently Json and Text implementations of this interface, and in
// #19546 an HTML version will be added to provide a hierarchical view.
class StatsRender {
public:
  virtual ~StatsRender() = default;

  // Writes a fragment for a numeric value, for counters and gauges.
  virtual void generate(Buffer::Instance& response, const std::string& name, uint64_t value) PURE;

  // Writes a json fragment for a textual value, for text readouts.
  virtual void generate(Buffer::Instance& response, const std::string& name,
                        const std::string& value) PURE;

  // Writes a histogram value.
  virtual void generate(Buffer::Instance& response, const std::string& name,
                        const Stats::ParentHistogram& histogram) PURE;

  // Completes rendering any buffered data.
  virtual void finalize(Buffer::Instance& response) PURE;

  // Indicates that no stats for a particular type have been found.
  virtual void noStats(Buffer::Instance&, absl::string_view) {}
};

// Implements the Render interface for simple textual representation of stats.
class StatsTextRender : public StatsRender {
public:
  explicit StatsTextRender(const StatsParams& params);

  // StatsRender
  void generate(Buffer::Instance& response, const std::string& name, uint64_t value) override;
  void generate(Buffer::Instance& response, const std::string& name,
                const std::string& value) override;
  void generate(Buffer::Instance& response, const std::string& name,
                const Stats::ParentHistogram& histogram) override;
  void finalize(Buffer::Instance&) override;

private:
  // Computes disjoint buckets as text and adds them to the response buffer.
  void addDisjointBuckets(const std::string& name, const Stats::ParentHistogram& histogram,
                          Buffer::Instance& response);

  void addDetail(const std::vector<Stats::ParentHistogram::Bucket>& buckets,
                 Buffer::Instance& response);

  const Utility::HistogramBucketsMode histogram_buckets_mode_;
};

// Implements the Render interface for json output.
class StatsJsonRender : public StatsRender {
public:
  StatsJsonRender(Http::ResponseHeaderMap& response_headers, Buffer::Instance& response,
                  const StatsParams& params);

  // StatsRender
  void generate(Buffer::Instance& response, const std::string& name, uint64_t value) override;
  void generate(Buffer::Instance& response, const std::string& name,
                const std::string& value) override;
  void generate(Buffer::Instance&, const std::string& name,
                const Stats::ParentHistogram& histogram) override;
  void finalize(Buffer::Instance& response) override;

private:
  using ProtoMap = Protobuf::Map<std::string, ProtobufWkt::Value>;

  // Summarizes the buckets in the specified histogram, collecting JSON objects.
  // Note, we do not flush this buffer to the network when it grows large, and
  // if this becomes an issue it should be possible to do, noting that we are
  // one or two levels nesting below the list of scalar stats due to the Envoy
  // stats json schema, where histograms are grouped together.
  void populateQuantiles(const Stats::ParentHistogram& histogram, absl::string_view label,
                         ProtobufWkt::ListValue* computed_quantile_value_array);

  // Collects the buckets from the specified histogram.
  void collectBuckets(const std::string& name, const Stats::ParentHistogram& histogram,
                      const std::vector<uint64_t>& interval_buckets,
                      const std::vector<uint64_t>& cumulative_buckets);

  void populateDetail(absl::string_view name,
                      const std::vector<Stats::ParentHistogram::Bucket>& buckets,
                      ProtoMap& histogram_obj_fields);
  void generateHistogramDetail(const std::string& name,
                               const Stats::ParentHistogram& histogram);
  void populateBucketsVerbose(const std::vector<Stats::ParentHistogram::Bucket>& buckets);
  void populateBucketsTerse(const std::vector<Stats::ParentHistogram::Bucket>& buckets);
  void renderHistogramStart();
  void populateSupportedPercentiles();
  void populatePercentiles(const Stats::ParentHistogram& histogram);

  const Utility::HistogramBucketsMode histogram_buckets_mode_;
  std::string name_buffer_;  // Used for Json::sanitize for names.
  std::string value_buffer_; // Used for Json::sanitize for text-readout values.
  bool histograms_initialized_{false};
  Json::Streamer json_streamer_;
  OptRef<Json::Streamer::Map> json_stats_map_;
  OptRef<Json::Streamer::Array> json_stats_array_;
  OptRef<Json::Streamer::Map> json_histogram_map1_;
  OptRef<Json::Streamer::Map> json_histogram_map2_;
  OptRef<Json::Streamer::Array> json_histogram_array_;
};

} // namespace Server
} // namespace Envoy
