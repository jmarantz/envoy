#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "envoy/common/pure.h"

#include "common/common/hash.h"

#include "absl/strings/string_view.h"

namespace Envoy {
namespace Stats {

class StatName;
//using StatNamePtr = std::unique_ptr<StatNameRef>;
class SymbolTable;
struct Tag;

/**
 * General interface for all stats objects.
 */
class Metric {
public:
  virtual ~Metric() {}
  /**
   * Returns the full name of the Metric.
   */
  virtual std::string name() const PURE;

  /**
   * Returns a reference to the name of the stat.
   */
  virtual StatName statName() const PURE;

  /**
   * Returns a vector of configurable tags to identify this Metric.
   */
  virtual std::vector<Tag> tags(const SymbolTable&) const PURE;

  /**
   * Returns the name of the Metric with the portions designated as tags removed.
   */
  virtual std::string tagExtractedName(const SymbolTable&) const PURE;

  /**
   * Indicates whether this metric has been updated since the server was started.
   */
  virtual bool used() const PURE;
};

/**
 * An always incrementing counter with latching capability. Each increment is added both to a
 * global counter as well as periodic counter. Calling latch() returns the periodic counter and
 * clears it.
 */
class Counter : public virtual Metric {
public:
  virtual ~Counter() {}
  virtual void add(uint64_t amount) PURE;
  virtual void inc() PURE;
  virtual uint64_t latch() PURE;
  virtual void reset() PURE;
  virtual uint64_t value() const PURE;
};

typedef std::shared_ptr<Counter> CounterSharedPtr;

/**
 * A gauge that can both increment and decrement.
 */
class Gauge : public virtual Metric {
public:
  virtual ~Gauge() {}

  virtual void add(uint64_t amount) PURE;
  virtual void dec() PURE;
  virtual void inc() PURE;
  virtual void set(uint64_t value) PURE;
  virtual void sub(uint64_t amount) PURE;
  virtual uint64_t value() const PURE;
};

typedef std::shared_ptr<Gauge> GaugeSharedPtr;

} // namespace Stats
} // namespace Envoy
