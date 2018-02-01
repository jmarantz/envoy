#include "common/stats/stats_impl.h"

#include <string.h>

#include <sys/time.h>
#include <unistd.h>
#include <cerrno>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>

#include "envoy/common/exception.h"

#include "common/common/utility.h"
#include "common/config/well_known_names.h"

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"

namespace Envoy {
namespace Stats {

namespace {

uint64_t NowUs() {
  struct timeval tv;
  struct timezone tz = { 0, 0 };  // UTC
  RELEASE_ASSERT(gettimeofday(&tv, &tz) == 0);
  return (static_cast<uint64_t>(tv.tv_sec) * 1000000) + tv.tv_usec;
}

struct RegexTimeCache {
  typedef std::map<std::string, int64_t> RegexTimeMap;

  void report(uint64_t start_time_us, const char* category, const std::string& description) {
    uint64_t end_time_us = NowUs();
    uint64_t duration_us = end_time_us - start_time_us;
    std::string key = absl::StrCat(category, " / ", description);
    {
      std::unique_lock<std::mutex> lock(mutex);
      regex_map[key] += duration_us;
    }
  }

  void Dump() {
    std::unique_lock<std::mutex> lock(mutex);
    for (const auto& p : regex_map) {
      std::cout << p.second << ": " << p.first << std::endl;
    }
  }

  static RegexTimeCache* getOrCreate() {
    static RegexTimeCache* cache = new RegexTimeCache;
    return cache;
  }

  RegexTimeMap regex_map;
  std::mutex mutex;
};

// Round val up to the next multiple of the natural alignment.
// Note: this implementation only works because 8 is a power of 2.
size_t roundUpMultipleNaturalAlignment(size_t val) {
  const size_t multiple = alignof(RawStatData);
  static_assert(multiple == 1 || multiple == 2 || multiple == 4 || multiple == 8 || multiple == 16,
                "multiple must be a power of 2 for this algorithm to work");
  return (val + multiple - 1) & ~(multiple - 1);
}

} // namespace

size_t RawStatData::size() {
  // Normally the compiler would do this, but because name_ is a flexible-array-length
  // element, the compiler can't. RawStatData is put into an array in HotRestartImpl, so
  // it's important that each element starts on the required alignment for the type.
  return roundUpMultipleNaturalAlignment(sizeof(RawStatData) + nameSize());
}

size_t& RawStatData::initializeAndGetMutableMaxObjNameLength(size_t configured_size) {
  // Like CONSTRUCT_ON_FIRST_USE, but non-const so that the value can be changed by tests
  static size_t size = configured_size;
  return size;
}

void RawStatData::configure(Server::Options& options) {
  const size_t configured = options.maxObjNameLength();
  RELEASE_ASSERT(configured > 0);
  size_t max_obj_name_length = initializeAndGetMutableMaxObjNameLength(configured);

  // If this fails, it means that this function was called too late during
  // startup because things were already using this size before it was set.
  RELEASE_ASSERT(max_obj_name_length == configured);
}

void RawStatData::configureForTestsOnly(Server::Options& options) {
  const size_t configured = options.maxObjNameLength();
  initializeAndGetMutableMaxObjNameLength(configured) = configured;
}

std::string Utility::sanitizeStatsName(const std::string& name) {
  std::string stats_name = name;
  std::replace(stats_name.begin(), stats_name.end(), ':', '_');
  return stats_name;
}

TagExtractorImpl::TagExtractorImpl(const std::string& name) : name_(name) {
}

TagExtractorRegexImpl::TagExtractorRegexImpl(const std::string& name, const std::string& regex)
    : TagExtractorImpl(name), prefix_(std::string(extractRegexPrefix(regex))),
      regex_(RegexUtil::parseRegex(regex)) {
}

std::string TagExtractorImpl::applyRemovals(const std::string& str,
                                            const IntervalSet& remove_characters) {
  std::string ret;
  int pos = 0;
  for (IntervalSet::Range range : remove_characters.toVector()) {
    if (range.first != pos) {
      ret += str.substr(pos, range.first - pos);
    }
    pos = range.second;
  }
  if (pos != static_cast<int>(str.size())) {
    ret += str.substr(pos);
  }

  return ret;
}

absl::string_view TagExtractorRegexImpl::extractRegexPrefix(absl::string_view regex) {
  absl::string_view::size_type start_pos = absl::StartsWith(regex, "^") ? 1 : 0;
  for (absl::string_view::size_type i = start_pos; i < regex.size(); ++i) {
    if (!absl::ascii_isalnum(regex[i]) && (regex[i] != '_')) {
      if (i > start_pos) {
        return regex.substr(0, i);
      }
      break;
    }
  }
  return absl::string_view(nullptr, 0);
}

TagExtractorPtr TagExtractorImpl::createTagExtractor(const std::string& name,
                                                     const std::string& regex) {
  if (name.empty()) {
    throw EnvoyException("tag_name cannot be empty");
  }

  if (regex.empty()) {
    throw EnvoyException(fmt::format(
        "No regex specified for tag specifier and no default regex for name: '{}'", name));
  }
  return TagExtractorPtr{new TagExtractorRegexImpl(name, regex)};

  /*
  // Look up the default for that name.
  const Config::TagNameValues::Descriptor* desc = Config::TagNames::get().find(name);
  if (desc == nullptr) {
    throw EnvoyException(fmt::format(
        "No regex specified for tag specifier and no default regex for name: '{}'", name));
  }
  return createTagExtractor(*desc);
  */
}

TagExtractorPtr TagExtractorImpl::createTagExtractor(
    const Config::TagNameValues::Descriptor& desc) {
  if (desc.is_regex) {
    return TagExtractorPtr{new TagExtractorRegexImpl(desc.name, desc.pattern)};
  } else {
    return TagExtractorPtr{new TagExtractorTokenImpl(desc.name, desc.pattern)};
  }
}

bool TagExtractorRegexImpl::extractTag(const std::string& stat_name, std::vector<Tag>& tags,
                                       IntervalSet& remove_characters) const {
  RegexTimeCache* cache = RegexTimeCache::getOrCreate();

  uint64_t start_time_us = NowUs();
  if (!prefix_.empty()) {
    if (prefix_[0] == '^') {
      if (!absl::StartsWith(stat_name, prefix_.substr(1))) {
        cache->report(start_time_us, "prefix-discard", name());
        return false;
      }
    } else if (absl::string_view(stat_name).find(prefix_) == absl::string_view::npos) {
      cache->report(start_time_us, "embedded-discard", name());
      return false;
    }
  }

  std::smatch match;
  // The regex must match and contain one or more subexpressions (all after the first are ignored).
  if (std::regex_search(stat_name, match, regex_) && match.size() > 1) {
    // remove_subexpr is the first submatch. It represents the portion of the string to be removed.
    const auto& remove_subexpr = match[1];

    // value_subexpr is the optional second submatch. It is usually inside the first submatch
    // (remove_subexpr) to allow the expression to strip off extra characters that should be removed
    // from the string but also not necessary in the tag value ("." for example). If there is no
    // second submatch, then the value_subexpr is the same as the remove_subexpr.
    const auto& value_subexpr = match.size() > 2 ? match[2] : remove_subexpr;

    tags.emplace_back();
    Tag& tag = tags.back();
    tag.name_ = name();
    tag.value_ = value_subexpr.str();

    // Reconstructs the tag_extracted_name without remove_subexpr.
    std::string::size_type start = remove_subexpr.first - stat_name.begin();
    std::string::size_type end = remove_subexpr.second - stat_name.begin();
    remove_characters.insert(start, end);
    cache->report(start_time_us, "success", name());
    return true;
  }
  cache->report(start_time_us, "miss", name());
  return false;
}

TagExtractorTokenImpl::TagExtractorTokenImpl(const std::string& name, const std::string& pattern)
    : TagExtractorImpl(name),
      tokens_(StringUtil::splitToken(pattern, ".", false)) {
  bool found = false;
  size_t tokens_size = tokens_.size();
  for (size_t i = 0; i < tokens_size; ++i) {
    RELEASE_ASSERT(!tokens_[i].empty());
    if (tokens_[i][0] == '$') {
      if (i != 0) {
        prefix_ = absl::StrJoin(&tokens_[0], &tokens_[i], ".") + ".";
      }
      found = true;
      break;
    }
  }
  if (!found) {
    // No variable field was found; this must be a simple exact match, so we can just
    // compare the input to the pattern.
    prefix_ = pattern;
  } else {
    for (size_t i = tokens_size; i >= 1; --i) {
      if (tokens_[i - 1][0] == '$') {
        if (i != tokens_size) {
          suffix_ = absl::StrCat(".", absl::StrJoin(&tokens_[i], &tokens_[tokens_size], "."));
        }
        found = true;
        break;
      }
    }
  }
}

bool TagExtractorTokenImpl::extractTag(const std::string& stat_name, std::vector<Tag>& tags,
                                       IntervalSet& remove_characters) const {
  RegexTimeCache* cache = RegexTimeCache::getOrCreate();

  uint64_t start_time_us = NowUs();
  if (!prefix_.empty() && !absl::StartsWith(stat_name, prefix_)) {
    cache->report(start_time_us, "Prefix-discard", name());
    return false;
  } else if (!suffix_.empty() && !absl::EndsWith(stat_name, suffix_)) {
    cache->report(start_time_us, "Suffix-discard", name());
    return false;
  }

  std::vector<absl::string_view> split_vec = StringUtil::splitToken(stat_name, ".");

  // In general a match may cover a span of tokens from split_vec.
  absl::string_view::size_type match_start_index = absl::string_view::npos;
  bool capture = false;
  std::string match;
  const auto capture_match = [&](absl::string_view::size_type pos) {
    if (match_start_index != absl::string_view::npos) {
      if (capture) {
        match = absl::StrJoin(&split_vec[match_start_index], &split_vec[pos], ".");
        capture = false;

        // TODO(jmarantz): if we eliminate the regex path completely we can specify
        // this range in terms of token indexes rather than characters, which will
        // eliminate this rather expensive calculation.
        std::string::size_type start =
            absl::StrJoin(&split_vec[0], &split_vec[match_start_index], ".").size();
        remove_characters.insert(start, start + match.size() + 1 /* leading dot */);
      }
      match_start_index = absl::string_view::npos;
    }
  };

  // In general, the number of tokens in the split will be >= the number of tokens in
  // the pattern, because some of the tokens may include addresses with embedded dots.
  // So we loop over the larger array, and conditionally advance over the smaller one.
  absl::string_view::size_type t = 0;
  absl::string_view::size_type num_tokens = tokens_.size();
  for (absl::string_view::size_type s = 0; s < split_vec.size() && t < num_tokens; ++s) {
    const absl::string_view split = split_vec[s];
    const absl::string_view token = tokens_[t];
    if (split == token) {  // TODO(jmarantz): make comparison aware of $$.
      capture_match(s);
      ++t;
    } else if (match_start_index == absl::string_view::npos) {
      // Here are the capture-keywords:
      //   $1 -- swallow one token
      //   $c1 -- copy one token
      //   $* -- greedily swallow N tokens until a literal match or dend of string.
      //   $c* -- greedily capture N tokens.
      if (token[0] == '$') {  // token guaranteed to be non-empty due to SkipWhitespace in ctor.
        ++t;
        if (token == "$1") {
          // nothing to do
        } else if (token == "$*") {
          match_start_index = s;
        } else {
          match_start_index = s;
          capture = true;
          if (token == "$c1") {
            capture_match(s + 1);
          } else {
            RELEASE_ASSERT(token == "$c*");
          }
        }
      } else {
        cache->report(start_time_us, "literal mismatch", name());
        return false;
      }
    }
  }
  if (t != num_tokens) {
    cache->report(start_time_us, "not enough tokens", name());
    return false;
  }
  if (match_start_index != absl::string_view::npos) {
    capture_match(split_vec.size());
  }
  if (!match.empty()) {
    tags.emplace_back();
    Tag& tag = tags.back();
    tag.name_ = name();
    tag.value_.swap(match);
  }
  cache->report(start_time_us, "Success", name());
  return true;
}

RawStatData* HeapRawStatDataAllocator::alloc(const std::string& name) {
  // This must be zero-initialized
  RawStatData* data = static_cast<RawStatData*>(::calloc(RawStatData::size(), 1));
  data->initialize(name);
  return data;
}

TagProducerImpl::TagProducerImpl(const envoy::api::v2::StatsConfig& config) : TagProducerImpl() {
  // To check name conflict.
  std::unordered_set<std::string> names;
  reserveResources(config);
  addDefaultExtractors(config, names);

  for (const auto& tag_specifier : config.stats_tags()) {
    if (!names.emplace(tag_specifier.tag_name()).second) {
      throw EnvoyException(fmt::format("Tag name '{}' specified twice.", tag_specifier.tag_name()));
    }

    // If no tag value is found, fallback to default regex to keep backward compatibility.
    if (tag_specifier.tag_value_case() == envoy::api::v2::TagSpecifier::TAG_VALUE_NOT_SET ||
        tag_specifier.tag_value_case() == envoy::api::v2::TagSpecifier::kRegex) {
      tag_extractors_.emplace_back(Stats::TagExtractorImpl::createTagExtractor(
          tag_specifier.tag_name(), tag_specifier.regex()));

    } else if (tag_specifier.tag_value_case() == envoy::api::v2::TagSpecifier::kFixedValue) {
      default_tags_.emplace_back(
          Stats::Tag{.name_ = tag_specifier.tag_name(), .value_ = tag_specifier.fixed_value()});
    }
  }
}

std::string TagProducerImpl::produceTags(const std::string& name, std::vector<Tag>& tags) const {
  tags.insert(tags.end(), default_tags_.begin(), default_tags_.end());

  IntervalSet remove_characters;
  bool needs_removal = false;
  for (const TagExtractorPtr& tag_extractor : tag_extractors_) {
    needs_removal |= tag_extractor->extractTag(name, tags, remove_characters);
  }
  if (!needs_removal) {
    return name;
  }
  return TagExtractorImpl::applyRemovals(name, remove_characters);
}

// Roughly estimate the size of the vectors.
void TagProducerImpl::reserveResources(const envoy::api::v2::StatsConfig& config) {
  default_tags_.reserve(config.stats_tags().size());

  /*
  if (!config.has_use_all_default_tags() || config.use_all_default_tags().value()) {
    tag_extractors_.reserve(Config::TagNames::get().name_regex_pairs_.size() +
                            config.stats_tags().size());
  } else {
    tag_extractors_.reserve(config.stats_tags().size());
  }
  */
}

void TagProducerImpl::addDefaultExtractors(const envoy::api::v2::StatsConfig& config,
                                           std::unordered_set<std::string>& names) {
  if (!config.has_use_all_default_tags() || config.use_all_default_tags().value()) {
    Config::TagNames::get().forEach([this, &names](const Config::TagNameValues::Descriptor& desc) {
        names.emplace(desc.name);
        tag_extractors_.emplace_back(Stats::TagExtractorImpl::createTagExtractor(desc));
      });
  }
}

void HeapRawStatDataAllocator::free(RawStatData& data) {
  // This allocator does not ever have concurrent access to the raw data.
  ASSERT(data.ref_count_ == 1);
  ::free(&data);
}

void RawStatData::initialize(absl::string_view key) {
  ASSERT(!initialized());
  ASSERT(key.size() <= maxNameLength());
  ASSERT(absl::string_view::npos == key.find(':'));
  ref_count_ = 1;

  // key is not necessarily nul-terminated, but we want to make sure name_ is.
  size_t xfer_size = std::min(nameSize() - 1, key.size());
  memcpy(name_, key.data(), xfer_size);
  name_[xfer_size] = '\0';
}

void DumpRegexStats() {
  return RegexTimeCache::getOrCreate()->Dump();
}

} // namespace Stats
} // namespace Envoy
