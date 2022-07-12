#include <memory>

#include "source/common/buffer/buffer_impl.h"
#include "source/common/stats/thread_local_store.h"
#include "source/server/admin/stats_render.h"

#include "test/mocks/event/mocks.h"
#include "test/mocks/stats/mocks.h"
#include "test/mocks/thread_local/mocks.h"
#include "test/test_common/utility.h"

#ifdef ENVOY_ADMIN_HTML
#include "source/server/admin/stats_html_render.h"
#endif

#include "gtest/gtest.h"

using testing::HasSubstr;
using testing::NiceMock;

namespace Envoy {
namespace Server {

class StatsRenderTest : public testing::Test {
protected:
  StatsRenderTest() : alloc_(symbol_table_), store_(alloc_) {
    store_.addSink(sink_);
    store_.initializeThreading(main_thread_dispatcher_, tls_);
  }

  ~StatsRenderTest() override {
    tls_.shutdownGlobalThreading();
    store_.shutdownThreading();
    tls_.shutdownThread();
  }

  template <class T>
  std::string render(StatsRender& render, absl::string_view name, const T& value) {
    render.generate(response_, std::string(name), value);
    render.finalize(response_);
    return response_.toString();
  }

  Stats::ParentHistogram& populateHistogram(const std::string& name,
                                            const std::vector<uint64_t>& vals) {
    Stats::Histogram& h = store_.histogramFromString(name, Stats::Histogram::Unit::Unspecified);
    for (uint64_t val : vals) {
      h.recordValue(val);
    }
    store_.mergeHistograms([]() -> void {});
    return dynamic_cast<Stats::ParentHistogram&>(h);
  }

  Stats::SymbolTableImpl symbol_table_;
  Stats::AllocatorImpl alloc_;
  NiceMock<Stats::MockSink> sink_;
  NiceMock<Event::MockDispatcher> main_thread_dispatcher_;
  NiceMock<ThreadLocal::MockInstance> tls_;
  Stats::ThreadLocalStoreImpl store_;
  Http::TestResponseHeaderMapImpl response_headers_;
  Buffer::OwnedImpl response_;
  StatsParams params_;
};

TEST_F(StatsRenderTest, TextInt) {
  StatsTextRender renderer(params_);
  EXPECT_EQ("name: 42\n", render<uint64_t>(renderer, "name", 42));
}

TEST_F(StatsRenderTest, TextString) {
  StatsTextRender renderer(params_);
  EXPECT_EQ("name: \"abc 123 ~!@#$%^&*()-_=+;:'\",<.>/?\"\n",
            render<std::string>(renderer, "name", "abc 123 ~!@#$%^&*()-_=+;:'\",<.>/?"));
}

TEST_F(StatsRenderTest, TextHistogramNoBuckets) {
  StatsTextRender renderer(params_);
  constexpr absl::string_view expected =
      "h1: P0(200,200) P25(207.5,207.5) P50(302.5,302.5) P75(306.25,306.25) "
      "P90(308.5,308.5) P95(309.25,309.25) P99(309.85,309.85) P99.5(309.925,309.925) "
      "P99.9(309.985,309.985) P100(310,310)\n";
  EXPECT_EQ(expected, render<>(renderer, "h1", populateHistogram("h1", {200, 300, 300})));
}

TEST_F(StatsRenderTest, TextHistogramCumulative) {
  params_.histogram_buckets_mode_ = Utility::HistogramBucketsMode::Cumulative;
  StatsTextRender renderer(params_);
  constexpr absl::string_view expected =
      "h1: B0.5(0,0) B1(0,0) B5(0,0) B10(0,0) B25(0,0) B50(0,0) B100(0,0) B250(1,1) "
      "B500(3,3) B1000(3,3) B2500(3,3) B5000(3,3) B10000(3,3) B30000(3,3) B60000(3,3) "
      "B300000(3,3) B600000(3,3) B1.8e+06(3,3) B3.6e+06(3,3)\n";
  EXPECT_EQ(expected, render<>(renderer, "h1", populateHistogram("h1", {200, 300, 300})));
}

TEST_F(StatsRenderTest, TextHistogramDisjoint) {
  params_.histogram_buckets_mode_ = Utility::HistogramBucketsMode::Disjoint;
  StatsTextRender renderer(params_);
  constexpr absl::string_view expected =
      "h1: B0.5(0,0) B1(0,0) B5(0,0) B10(0,0) B25(0,0) B50(0,0) B100(0,0) B250(1,1) B500(2,2) "
      "B1000(0,0) B2500(0,0) B5000(0,0) B10000(0,0) B30000(0,0) B60000(0,0) B300000(0,0) "
      "B600000(0,0) B1.8e+06(0,0) B3.6e+06(0,0)\n";
  EXPECT_EQ(expected, render<>(renderer, "h1", populateHistogram("h1", {200, 300, 300})));
}

TEST_F(StatsRenderTest, JsonInt) {
  StatsJsonRender renderer(response_headers_, response_, params_);
  const std::string expected = R"EOF({"stats":[ {"value":42, "name":"name"}]})EOF";
  EXPECT_THAT(render<uint64_t>(renderer, "name", 42), JsonStringEq(expected));
}

TEST_F(StatsRenderTest, JsonString) {
  StatsJsonRender renderer(response_headers_, response_, params_);
  const std::string expected = R"EOF({
    "stats": [{"value": "abc 123 ~!@#$%^&*()-_=+;:'\",\u003c.\u003e/?",
               "name": "name"}]})EOF";
  EXPECT_THAT(render<std::string>(renderer, "name", "abc 123 ~!@#$%^&*()-_=+;:'\",<.>/?"),
              JsonStringEq(expected));
}

TEST_F(StatsRenderTest, JsonHistogramNoBuckets) {
  StatsJsonRender renderer(response_headers_, response_, params_);
  const std::string expected = R"EOF(
{
    "stats": [{
        "histograms": {
            "supported_quantiles": [0, 25, 50, 75, 90, 95, 99, 99.5, 99.9, 100],
            "computed_quantiles": [{
                "values": [{
                    "cumulative": 200,
                    "interval": 200
                }, {
                    "interval": 207.5,
                    "cumulative": 207.5
                }, {
                    "interval": 302.5,
                    "cumulative": 302.5
                }, {
                    "cumulative": 306.25,
                    "interval": 306.25
                }, {
                    "cumulative": 308.5,
                    "interval": 308.5
                }, {
                    "cumulative": 309.25,
                    "interval": 309.25
                }, {
                    "interval": 309.85,
                    "cumulative": 309.85
                }, {
                    "cumulative": 309.925,
                    "interval": 309.925
                }, {
                    "interval": 309.985,
                    "cumulative": 309.985
                }, {
                    "cumulative": 310,
                    "interval": 310
                }],
                "name": "h1"
            }]
        }
    }]
}
  )EOF";
  EXPECT_THAT(render<>(renderer, "h1", populateHistogram("h1", {200, 300, 300})),
              JsonStringEq(expected));
}

TEST_F(StatsRenderTest, JsonHistogramCumulative) {
  params_.histogram_buckets_mode_ = Utility::HistogramBucketsMode::Cumulative;
  StatsJsonRender renderer(response_headers_, response_, params_);
  const std::string expected = R"EOF(
{
    "stats": [{
        "histograms": [{
            "name": "h1",
            "buckets": [{
                "upper_bound": 0.5,
                "interval": 0,
                "cumulative": 0
            }, {
                "upper_bound": 1,
                "interval": 0,
                "cumulative": 0
            }, {
                "interval": 0,
                "upper_bound": 5,
                "cumulative": 0
            }, {
                "interval": 0,
                "upper_bound": 10,
                "cumulative": 0
            }, {
                "interval": 0,
                "upper_bound": 25,
                "cumulative": 0
            }, {
                "cumulative": 0,
                "upper_bound": 50,
                "interval": 0
            }, {
                "upper_bound": 100,
                "interval": 0,
                "cumulative": 0
            }, {
                "upper_bound": 250,
                "cumulative": 1,
                "interval": 1
            }, {
                "upper_bound": 500,
                "interval": 3,
                "cumulative": 3
            }, {
                "cumulative": 3,
                "upper_bound": 1000,
                "interval": 3
            }, {
                "upper_bound": 2500,
                "cumulative": 3,
                "interval": 3
            }, {
                "cumulative": 3,
                "interval": 3,
                "upper_bound": 5000
            }, {
                "upper_bound": 10000,
                "cumulative": 3,
                "interval": 3
            }, {
                "upper_bound": 30000,
                "interval": 3,
                "cumulative": 3
            }, {
                "cumulative": 3,
                "upper_bound": 60000,
                "interval": 3
            }, {
                "interval": 3,
                "upper_bound": 300000,
                "cumulative": 3
            }, {
                "upper_bound": 600000,
                "interval": 3,
                "cumulative": 3
            }, {
                "interval": 3,
                "cumulative": 3,
                "upper_bound": 1800000
            }, {
                "upper_bound": 3600000,
                "interval": 3,
                "cumulative": 3
            }]
        }]
    }]
}
  )EOF";
  EXPECT_THAT(render<>(renderer, "h1", populateHistogram("h1", {200, 300, 300})),
              JsonStringEq(expected));
}

TEST_F(StatsRenderTest, JsonHistogramDisjoint) {
  params_.histogram_buckets_mode_ = Utility::HistogramBucketsMode::Disjoint;
  StatsJsonRender renderer(response_headers_, response_, params_);
  const std::string expected = R"EOF(
{
    "stats": [{
        "histograms": [{
            "buckets": [{
                "interval": 0,
                "cumulative": 0,
                "upper_bound": 0.5
            }, {
                "cumulative": 0,
                "interval": 0,
                "upper_bound": 1
            }, {
                "cumulative": 0,
                "interval": 0,
                "upper_bound": 5
            }, {
                "upper_bound": 10,
                "interval": 0,
                "cumulative": 0
            }, {
                "interval": 0,
                "upper_bound": 25,
                "cumulative": 0
            }, {
                "cumulative": 0,
                "upper_bound": 50,
                "interval": 0
            }, {
                "cumulative": 0,
                "upper_bound": 100,
                "interval": 0
            }, {
                "interval": 1,
                "cumulative": 1,
                "upper_bound": 250
            }, {
                "cumulative": 2,
                "upper_bound": 500,
                "interval": 2
            }, {
                "interval": 0,
                "cumulative": 0,
                "upper_bound": 1000
            }, {
                "interval": 0,
                "cumulative": 0,
                "upper_bound": 2500
            }, {
                "interval": 0,
                "upper_bound": 5000,
                "cumulative": 0
            }, {
                "upper_bound": 10000,
                "interval": 0,
                "cumulative": 0
            }, {
                "cumulative": 0,
                "interval": 0,
                "upper_bound": 30000
            }, {
                "upper_bound": 60000,
                "interval": 0,
                "cumulative": 0
            }, {
                "upper_bound": 300000,
                "interval": 0,
                "cumulative": 0
            }, {
                "cumulative": 0,
                "upper_bound": 600000,
                "interval": 0
            }, {
                "upper_bound": 1800000,
                "interval": 0,
                "cumulative": 0
            }, {
                "interval": 0,
                "cumulative": 0,
                "upper_bound": 3600000
            }],
            "name": "h1"
        }]
    }]
}
  )EOF";
  EXPECT_THAT(render<>(renderer, "h1", populateHistogram("h1", {200, 300, 300})),
              JsonStringEq(expected));
}

#ifdef ENVOY_ADMIN_HTML
class StatsHtmlRenderTest : public StatsRenderTest {
protected:
  const Admin::UrlHandler url_handler_{
      "/foo", "help", [](absl::string_view, AdminStream&) -> Admin::RequestPtr { return nullptr; },
      false, false};
  StatsHtmlRender renderer_{response_headers_, response_, url_handler_, params_};
};

TEST_F(StatsHtmlRenderTest, String) {
  EXPECT_THAT(render<std::string>(renderer_, "name", "abc 123 ~!@#$%^&*()-_=+;:'\",<.>/?"),
              HasSubstr("name: \"abc 123 ~!@#$%^&amp;*()-_=+;:&#39;&quot;,&lt;.&gt;/?\"\n"));
}

TEST_F(StatsHtmlRenderTest, HistogramNoBuckets) {
  constexpr absl::string_view expected =
      "h1: P0(200,200) P25(207.5,207.5) P50(302.5,302.5) P75(306.25,306.25) "
      "P90(308.5,308.5) P95(309.25,309.25) P99(309.85,309.85) P99.5(309.925,309.925) "
      "P99.9(309.985,309.985) P100(310,310)\n";
  EXPECT_THAT(render<>(renderer_, "h1", populateHistogram("h1", {200, 300, 300})),
              HasSubstr(expected));
}
#endif

} // namespace Server
} // namespace Envoy
