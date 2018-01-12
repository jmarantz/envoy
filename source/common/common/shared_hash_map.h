#pragma once

#include <pthread.h>

#include <string>

#include "common/common/hash.h"
#include "common/common/logger.h"

#include "absl/base/thread_annotations.h"
#include "absl/strings/string_view.h"
#include "fmt/format.h"

namespace Envoy {

// This block is used solely to help with initialization. It is duplicated
// to the control-block after init.
struct SharedHashMapOptions {
  std::string toString() const {
    return fmt::format("capacity={}, num_string_bytes={}, num_slots={}", capacity, num_string_bytes,
                       num_slots);
  }

  uint32_t capacity;         // how many values can be stored.
  uint32_t num_string_bytes; // how many bytes of string can be stored.
  uint32_t num_slots;        // determines speed of hash vs size efficiency.
};

/**
 * Implements Hash-map<string, Value> without using pointers, suitable
 * for use in shared memory. This hash-table exploits a simplification
 * for its intended use-case (Envoy stats); it provides no way to
 * delete a key. If we did want to delete keys we'd have to be able
 * to recycle string memory, which would probably force us to save uniform
 * key sizes.
 *
 * This map may also be suitable for a persistent memory-mapped hash-table.
 */
template <class Value> class SharedHashMap : public Logger::Loggable<Logger::Id::config> {
public:
  /**
   * Sentinal to used for a cell's char_offset to indicate it's free.
   * We also use this sentinal to denote the end of the free-list of cells.
   */
  static const uint32_t Sentinal = 0xffffffff;

  /**
   * Constructs a map control structure given a set of options, which cannot be changed.
   * Note that the map dtaa itself is not constructed when the object is constructed.
   * After the control-structure is constructed, the number of bytes can be computed so
   * that a shared-memory segment can be allocated and passed to init() or attach().
   */
  SharedHashMap(const SharedHashMapOptions& options)
      : options_(options), control_(nullptr), slots_(nullptr), cells_(nullptr), chars_(nullptr) {}

  /**
   * Represents control-values for the hash-table, including a mutex, which
   * must gate all access to the internas.
   */
  struct Control {
    std::string toString() const {
      return fmt::format("{} size={} next_key_char={}", options.toString(), size, next_key_char);
    }

    mutable pthread_mutex_t mutex; // Marked mutable so get() can be const and also lock.
    SharedHashMapOptions options;  // Options established at map construction time.
    uint32_t size;                 // Number of values currently stored.
    uint32_t next_key_char;        // Offset of next key in chars_.
  };

  /**
   * Represents a value-cell, which is stored in a linked-list from each slot.
   */
  struct Cell {
    void free() { char_offset = Sentinal; }
    absl::string_view key(const char* chars) const {
      return absl::string_view(&chars[char_offset + 1], chars[char_offset]);
    }

    uint32_t char_offset; // Offset of the key bytes into map->chars_.
    uint32_t next_cell;   // OFfset of next cell in map->cells_, terminated with Sentinal.
    Value value;          // Templated value field.
  };

  /** Returns the numbers of byte required for the hash-table, based on the control structure. */
  size_t numBytes() const {
    return sizeof(Control) + (options_.num_slots * sizeof(uint32_t)) +
           (options_.capacity * sizeof(Cell)) + options_.num_string_bytes;
  }

  /**
   * Attempts to attach to an existing shared memory segment. Does a (relatively) quick
   * sanity check to make sure the options copied to the provided memory match, and also
   * that the slot, cell, and key-string structures look sane.
   *
   * Note that if mutex is in a locked state at the time of attachment, this function
   * can hang.
   */
  bool attach(uint8_t* memory) {
    initHelper(memory);
    return sanityCheck();
  }

  /** Locks the map and runs sanity checks */
  bool sanityCheck() LOCKS_EXCLUDED(control_->mutex) {
    lock(); // might hang if program previously crashed.
    bool ret = sanityCheckLockHeld();
    unlock();
    return ret;
  }

  /**
   * Returns a string describing the contents of the map, including the control
   * bits and the keys in each slot.
   */
  std::string toString() const {
    std::string ret;
    lock();
    ret = fmt::format("options={}\ncontrol={}\n", options_.toString(), control_->toString());
    for (uint32_t i = 0; i < options_.num_slots; ++i) {
      ret += fmt::format("slot {}:", i);
      for (uint32_t j = slots_[i]; j != Sentinal; j = cells_[j].next_cell) {
        const Cell* cell = &cells_[j];
        std::string key(std::string(cell->key(chars_)));
        ret += " " + key;
      }
      ret += "\n";
    }
    unlock();
    return ret;
  }

  /**
   * Initializes a hash-map on raw memory. No expectations are made about the state of the memory
   * coming in.
   * @param memory
   */
  void init(uint8_t* memory) {
    initHelper(memory);
    pthread_mutexattr_t attribute;
    pthread_mutexattr_init(&attribute);
    pthread_mutexattr_setpshared(&attribute, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&attribute, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(&control_->mutex, &attribute);
    lock();

    control_->options = options_;
    control_->size = 0;
    control_->next_key_char = 0;

    // Initialize all the slots;
    for (uint32_t i = 0; i < options_.num_slots; ++i) {
      slots_[i] = Sentinal;
    }

    // Initialize all the key-char offsets.
    for (uint32_t i = 0; i < options_.capacity; ++i) {
      cells_[i].char_offset = Sentinal;
    }

    unlock();
  }

  /**
   * Puts a new key into the map. If successful (e.g. map has capacity)
   * then put returns a pointer to the value object, which the caller
   * can then write. Returns nullptr if the key was too large, or the
   * capacity of the map has been exceeded.
   *
   * @param key THe key must be 255 bytes or smaller.
   */
  Value* put(absl::string_view key) {
    Value* value = nullptr;
    lock();
    if ((key.size() <= 255) && (control_->size < options_.capacity) &&
        (key.size() + 1 + control_->next_key_char <= options_.num_string_bytes)) {
      uint32_t slot = HashUtil::xxHash64(key) % options_.num_slots;

      uint32_t* prevNext = &slots_[slot];
      uint32_t cell = *prevNext;

      while (cell != Sentinal) {
        prevNext = &cells_[cell].next_cell;
        cell = *prevNext;
      }

      cell = control_->size++;
      *prevNext = cell;
      value = &cells_[cell].value;
      uint32_t char_offset = control_->next_key_char;
      cells_[cell].char_offset = char_offset;
      cells_[cell].next_cell = Sentinal;
      control_->next_key_char += key.size() + 1;
      chars_[char_offset] = key.size();
      memcpy(&chars_[char_offset + 1], key.data(), key.size());
    }
    unlock();
    return value;
  }

  /** Returns the number of key/values stored in the map. */
  size_t size() const { return control_->size; }

  /**
   * Const method to get a value.
   * @param key
   */
  const Value* get(absl::string_view key) const {
    SharedHashMap* non_const_this = const_cast<SharedHashMap*>(this);
    return non_const_this->get(key);
  }

  /**
   * Gets the value associated with a key, returning null if the value was not found.
   * @param key
   */
  Value* get(absl::string_view key) {
    lock();
    Value* value = nullptr;
    if (key.size() <= 255) {
      uint32_t slot = HashUtil::xxHash64(key) % options_.num_slots;
      for (uint32_t cell = slots_[slot]; cell != Sentinal; cell = cells_[cell].next_cell) {
        absl::string_view cell_key = cells_[cell].key(chars_);
        if (cell_key == key) {
          value = &cells_[cell].value;
          break;
        }
      }
    }
    unlock();
    return value;
  }

private:
  /** Maps out the segments of shared memory for us to work with. */
  void initHelper(uint8_t* memory) {
    // Note that we are not examining or mutating memory here, just looking at the pointer,
    // so we don't need to hold any locks.
    control_ = reinterpret_cast<Control*>(memory);
    memory += sizeof(Control);
    slots_ = reinterpret_cast<uint32_t*>(memory);
    memory += options_.num_slots * sizeof(uint32_t);
    cells_ = reinterpret_cast<Cell*>(memory);
    memory += options_.capacity * sizeof(Cell);
    chars_ = reinterpret_cast<char*>(memory);
  }

  /** Examines the data structures to see if they are sane. Tries not to crash or hang. */
  bool sanityCheckLockHeld() EXCLUSIVE_LOCKS_REQUIRED(control_->mutex) {
    bool ret = true;
    if (memcmp(&options_, &control_->options, sizeof(SharedHashMapOptions)) != 0) {
      // options doesn't match.
      ENVOY_LOG(error, "SharedMap options don't match");
      return false;
    }

    if (control_->size > options_.capacity) {
      ENVOY_LOG(error, "SharedMap size={} > capacity={}", control_->size, options_.capacity);
      return false;
    }

    // As a sanity check, makee sure there are control_->size values
    // reachable from the slots, each of which has a valid char_offset
    uint32_t num_values = 0;
    for (uint32_t i = 0; i < options_.num_slots; ++i) {
      for (uint32_t j = slots_[i]; j != Sentinal; j = cells_[j].next_cell) {
        if (j >= options_.capacity) {
          ENVOY_LOG(error, "SharedMap live cell has corrupt next_cell");
          ret = false;
        } else {
          uint32_t char_offset = cells_[j].char_offset;
          if (char_offset == Sentinal) {
            ENVOY_LOG(error, "SharedMap live cell has char_offset==Sentinal");
            ret = false;
          } else if (char_offset >= options_.num_string_bytes) {
            ENVOY_LOG(error, "SharedMap live cell has corrupt_offset: {}", char_offset);
            ret = false;
          } else {
            ++num_values;
            if (num_values > control_->size) { // avoid infinite loops if there is a bucket cycle.
              break;
            }
          }
        }
      }
    }
    if (num_values != control_->size) {
      ENVOY_LOG(error, "SharedMap has wrong number of live cells: {}, expected {}", num_values,
                control_->size);
      ret = false;
    }
    return ret;
  }

  /** Locks the mutex. */
  void lock() const { pthread_mutex_lock(&control_->mutex); }

  /** Unocks the mutex. */
  void unlock() const { pthread_mutex_unlock(&control_->mutex); }

  const SharedHashMapOptions options_;

  // Pointers into shared memory.
  Control* control_;
  uint32_t* slots_ PT_GUARDED_BY(control_->mutex);
  Cell* cells_ PT_GUARDED_BY(control_->mutex);
  char* chars_ PT_GUARDED_BY(control_->mutex);
};

} // namespace Envoy
