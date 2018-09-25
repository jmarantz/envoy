#include "common/stats/metric_impl.h"

#include "envoy/stats/tag.h"

#include "common/stats/symbol_table_impl.h"

namespace Envoy {
namespace Stats {

MetricImpl::MetricImpl(std::string&& tag_extracted_name, std::vector<Tag>&& tags,
                       SymbolTable& symbol_table) {
  ASSERT(tags.size() < 256);
  SymbolVec name = symbol_table.encode(tag_extracted_name);
  size_t total_size = StatName::size(name) + 1;
  std::vector<std::pair<SymbolVec, SymbolVec>> sym_vecs;
  sym_vecs.reserve(tags.size());
  for (auto tag : tags) {
    sym_vecs.emplace_back(std::make_pair(
        symbol_table.encode(tag.name_), symbol_table.encode(tag.value_)));
    total_size += StatName::size(sym_vecs.back().first) +
                  StatName::size(sym_vecs.back().second);
  }
  storage_.reset(new uint8_t[total_size]);
  uint8_t* p = &storage_[0];
  *p++ = tags.size();
  p += StatName().init(name, p);
  for (auto& sym_vec_pair : sym_vecs) {
    StatName name, value;
    p += name.init(sym_vec_pair.first, p);
    p += value.init(sym_vec_pair.second, p);
  }
  ASSERT(p == &storage_[0] + total_size);
}

std::vector<Tag> MetricImpl::tags(const SymbolTable& symbol_table) const {
  uint8_t* p = &storage_[0];
  size_t num_tags = *p++;
  std::vector<Tag> tags;
  tags.reserve(num_tags);
  StatName stat_name(p);
  p += stat_name.sizeBytes();

  for (size_t i = 0; i < num_tags; ++i) {
    Tag tag;
    StatName name(p);
    tag.name_ = name.toString(symbol_table);
    p += name.sizeBytes();
    StatName value(p);
    tag.value_ = value.toString(symbol_table);
    p += value.sizeBytes();
  }
  return tags;
}

} // namespace Stats
} // namespace Envoy
