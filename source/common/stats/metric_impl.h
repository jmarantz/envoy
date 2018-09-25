#pragma once

#include <string>
#include <vector>

#include "envoy/stats/stats.h"

#include "common/common/assert.h"
#include "common/stats/symbol_table_impl.h"

namespace Envoy {
namespace Stats {

/**
 * Implementation of the Metric interface. Virtual inheritance is used because the interfaces that
 * will inherit from Metric will have other base classes that will also inherit from Metric.
 *
 * MetricImpl is not meant to be instantiated as-is. For performance reasons we keep name() virtual
 * and expect child classes to implement it.
 */
class MetricImpl : public virtual Metric {
public:
  MetricImpl(std::string&& tag_extracted_name, std::vector<Tag>&& tags, SymbolTable& symbol_table);

  std::string tagExtractedName(const SymbolTable& symbol_table) const override {
    StatName stat_name(&storage_[0]);
    return stat_name.toString(symbol_table);
  }
  std::vector<Tag> tags(const SymbolTable&) const override;

protected:
  /**
   * Flags used by all stats types to figure out whether they have been used.
   */
  struct Flags {
    static const uint8_t Used = 0x1;
  };

private:
  //StatName tagExtractedStatName() { return StatName(storage_); }

  std::vector<std::pair<StatName, StatName>> tags_;
  std::unique_ptr<uint8_t[]> storage_;
};

} // namespace Stats
} // namespace Envoy
