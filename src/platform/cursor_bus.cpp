/**
 * @file src/platform/cursor_bus.cpp
 * @brief Shell: definitions for the cursor bus (see cursor_bus.h).
 */
// standard includes
#include <atomic>
#include <mutex>

// local includes
#include "cursor_bus.h"
#include "src/logging.h"

namespace platf::cursor_bus {
  namespace {
    struct bus_t {
      std::mutex mutex;
      cursor_shape::raw_t raw;
      std::uint64_t shape_gen = 0;
      std::uint64_t state_gen = 0;
      bool visible = true;

      // Conversion cache (the generation it belongs to, and the result; null when not convertible)
      std::uint64_t converted_gen = 0;
      std::shared_ptr<const cursor_shape::bgra_t> converted;
      bool oversize_logged = false;
    };

    bus_t &bus() {
      static bus_t instance;
      return instance;
    }

    std::atomic<bool> shapes_available_flag {true};
    std::atomic<bool> compose_flag {true};
  }  // namespace

  void publish_shape(cursor_shape::raw_t raw) {
    auto &b = bus();
    std::lock_guard lock {b.mutex};
    b.raw = std::move(raw);
    ++b.shape_gen;
  }

  void publish_visible(bool visible) {
    auto &b = bus();
    std::lock_guard lock {b.mutex};
    if (b.visible != visible) {
      b.visible = visible;
      ++b.state_gen;
    }
  }

  void set_shapes_available(bool available) {
    shapes_available_flag.store(available, std::memory_order_relaxed);
  }

  bool shapes_available() {
    return shapes_available_flag.load(std::memory_order_relaxed);
  }

  snapshot_t snapshot() {
    auto &b = bus();
    std::lock_guard lock {b.mutex};

    if (b.converted_gen != b.shape_gen) {
      // At most 256x256 pixels: well under a millisecond, and only once per new shape
      auto shape = std::make_shared<cursor_shape::bgra_t>();
      switch (cursor_shape::convert(b.raw, *shape)) {
        case cursor_shape::convert_e::ok:
          b.converted = std::move(shape);
          break;
        case cursor_shape::convert_e::oversize:
          b.converted.reset();
          if (!b.oversize_logged) {
            b.oversize_logged = true;
            BOOST_LOG(warning) << "Local cursor: pointer shape " << b.raw.width << 'x' << (b.raw.type == cursor_shape::type_monochrome ? b.raw.height / 2 : b.raw.height)
                               << " exceeds " << cursor_shape::max_dimension << 'x' << cursor_shape::max_dimension << " and is not sent to clients (they show their default arrow)";
          }
          break;
        case cursor_shape::convert_e::invalid:
          b.converted.reset();
          BOOST_LOG(warning) << "Local cursor: pointer shape of type " << b.raw.type << " (" << b.raw.width << 'x' << b.raw.height << ", pitch " << b.raw.pitch << ", " << b.raw.data.size() << " bytes) cannot be converted";
          break;
      }
      b.converted_gen = b.shape_gen;
    }

    snapshot_t snap;
    snap.shape_gen = b.shape_gen;
    snap.state_gen = b.state_gen;
    snap.visible = b.visible;
    snap.shape = b.converted;
    return snap;
  }

  std::shared_ptr<const cursor_shape::bgra_t> find_shape(std::uint32_t id) {
    auto snap = snapshot();
    if (snap.shape && snap.shape->id == id) {
      return snap.shape;
    }
    return nullptr;
  }

  bool compose() {
    return compose_flag.load(std::memory_order_relaxed);
  }

  void set_compose(bool on) {
    compose_flag.store(on, std::memory_order_relaxed);
  }
}  // namespace platf::cursor_bus
