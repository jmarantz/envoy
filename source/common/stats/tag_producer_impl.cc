#include "source/common/stats/tag_producer_impl.h"

#include <string>

#include "envoy/common/exception.h"
#include "envoy/config/metrics/v3/stats.pb.h"

#include "source/common/common/utility.h"
#include "source/common/stats/tag_extractor_impl.h"
#include "source/common/stats/utility.h"

namespace Envoy {
namespace Stats {

TagProducerImpl::TagProducerImpl(const envoy::config::metrics::v3::StatsConfig& config,
                                 SymbolTable& symbol_table)
    : symbol_table_(symbol_table) {
  // To check name conflict.
  reserveResources(config);
  absl::node_hash_set<std::string> names = addDefaultExtractors(config);

  for (const auto& tag_specifier : config.stats_tags()) {
    const std::string& name = tag_specifier.tag_name();
    if (!names.emplace(name).second) {
      throw EnvoyException(fmt::format("Tag name '{}' specified twice.", name));
    }

    // If no tag value is found, fallback to default regex to keep backward compatibility.
    if (tag_specifier.tag_value_case() ==
            envoy::config::metrics::v3::TagSpecifier::TagValueCase::TAG_VALUE_NOT_SET ||
        tag_specifier.tag_value_case() ==
            envoy::config::metrics::v3::TagSpecifier::TagValueCase::kRegex) {

      if (tag_specifier.regex().empty()) {
        if (addExtractorsMatching(name) == 0) {
          throw EnvoyException(fmt::format(
              "No regex specified for tag specifier and no default regex for name: '{}'", name));
        }
      } else {
        addExtractor(TagExtractorImplBase::createTagExtractor(name, tag_specifier.regex()));
      }
    } else if (tag_specifier.tag_value_case() ==
               envoy::config::metrics::v3::TagSpecifier::TagValueCase::kFixedValue) {
      default_tags_.emplace_back(Tag{name, tag_specifier.fixed_value()});
    }
  }
}

TagProducerImpl::TagProducerImpl(SymbolTable& symbol_table) : symbol_table_(symbol_table) {}

int TagProducerImpl::addExtractorsMatching(absl::string_view name) {
  int num_found = 0;
  for (const auto& desc : Config::TagNames::get().descriptorVec()) {
    if (desc.name_ == name) {
      addExtractor(TagExtractorImplBase::createTagExtractor(desc.name_, desc.regex_, desc.substr_,
                                                            desc.re_type_));
      ++num_found;
    }
  }
  for (const auto& desc : Config::TagNames::get().tokenizedDescriptorVec()) {
    if (desc.name_ == name) {
      addExtractor(std::make_unique<TagExtractorTokensImpl>(desc.name_, desc.pattern_));
      ++num_found;
    }
  }
  return num_found;
}

void TagProducerImpl::addExtractor(TagExtractorPtr extractor) {
  const absl::string_view prefix = extractor->prefixToken();
  if (prefix.empty()) {
    tag_extractors_without_prefix_.emplace_back(std::move(extractor));
  } else {
    tag_extractor_prefix_map_[prefix].emplace_back(std::move(extractor));
  }
}

void TagProducerImpl::forEachExtractorMatching(
    absl::string_view stat_name, std::function<void(const TagExtractorPtr&)> f) const {
  for (const TagExtractorPtr& tag_extractor : tag_extractors_without_prefix_) {
    f(tag_extractor);
  }
  const absl::string_view::size_type dot = stat_name.find('.');
  if (dot != std::string::npos) {
    const absl::string_view token = absl::string_view(stat_name.data(), dot);
    const auto iter = tag_extractor_prefix_map_.find(token);
    if (iter != tag_extractor_prefix_map_.end()) {
      for (const TagExtractorPtr& tag_extractor : iter->second) {
        f(tag_extractor);
      }
    }
  }
}

std::string TagProducerImpl::produceTags(TagExtractionContext& extraction_context) const {
  // TODO(jmarantz): Skip the creation of string-based tags, creating a StatNameTagVector instead.
  TagVector& tags = extraction_context.tags();
  tags.insert(tags.end(), default_tags_.begin(), default_tags_.end());
  forEachExtractorMatching(extraction_context.name(),
                           [&extraction_context](const TagExtractorPtr& tag_extractor) {
                             tag_extractor->extractTag(extraction_context);
                           });
  return extraction_context.tagExtractedName();
}

class TagFromStatNameContext {
 public:
  using Segment = absl::variant<Element, absl::string_view>;
  using SegmentVec = std::vector<Segment>;

  TagFromStatNameContext(StatName metric_name, StatNameTagVector& tags,
                         StatName& tag_extracted_name, StatNamePool& pool)
      : tags_(tags), tag_extracted_name_(tag_extracted_name), pool_(pool) {
    pool.symbolTable().decode(metric_name,
                              [this](Symbol symbol) { segment_vec_.push_back(Segment(symbol)); },
                              [this](absl::string_view name) {
                                segment_vec_.push_back(Segment(name));
                              });
  }

  StatNameTagVector& tags_;
  StatName& tag_extracted_name_;
  StatNamePool& pool_;
  SegmentVec segment_vec_;
};

bool TagProducerImpl::produceTagsFromStatNameHelper(
    StatName metric_name, StatNameTagVector& tags,
    StatName& tag_extracted_name,
    StatNamePool& pool) const {
}

bool TagProducerImpl::produceTagsFromStatName(StatName metric_name, StatNameTagVector& tags,
                                              StatName& tag_extracted_name,
                                              StatNamePool& pool) const {
  bool mismatch = false;
  std::set<uint32_t> tag_indices;
  uint32_t index = 0;
  nodes.push_back(token_root_.get());
  SegmentVec v;


    if (mismatch) {
      return;
    }
    std::vector<TokenNode*> new_nodes;
    for (TokenNode* node : nodes) {
      if (node.next_.empty()) {
        // This is a terminal node
        tag_indices.insert(index);
        tags.emplace_back(StatNameTag{
      } else {
        auto iter = node->next_.find(symbol);
        if (iter != node->next_.end()) {
          for (std::unique_ptr<TokenNode>& node = iter->second) {
            new_nodes.push_back(node.get());
          }
        }
      }
    }
    if (new_nodes.empty()) {
      mismatch = true;
      return;
    }
      for
    const std::vector<std::unique_ptr<TokenNode>>& matches = next_.
  }
  return false;
}

void TagProducerImpl::reserveResources(const envoy::config::metrics::v3::StatsConfig& config) {
  default_tags_.reserve(config.stats_tags().size());
}

absl::node_hash_set<std::string>
TagProducerImpl::addDefaultExtractors(const envoy::config::metrics::v3::StatsConfig& config) {
  absl::node_hash_set<std::string> names;
  if (!config.has_use_all_default_tags() || config.use_all_default_tags().value()) {
    for (const auto& desc : Config::TagNames::get().descriptorVec()) {
      names.emplace(desc.name_);
      addExtractor(TagExtractorImplBase::createTagExtractor(desc.name_, desc.regex_, desc.substr_,
                                                            desc.re_type_));
    }
    for (const auto& desc : Config::TagNames::get().tokenizedDescriptorVec()) {
      names.emplace(desc.name_);
      addExtractor(std::make_unique<TagExtractorTokensImpl>(desc.name_, desc.pattern_));
    }
  }
  return names;
}

} // namespace Stats
} // namespace Envoy
