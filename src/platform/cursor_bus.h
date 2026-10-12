/**
 * @file src/platform/cursor_bus.h
 * @brief Shell: the cursor bus, the latest mouse pointer shape and visibility as the capture
 *        reports them, for clients that draw the cursor themselves (docs/features.md "Local cursor").
 *
 * One global, because one capture thread serves every streaming session. The capture thread only
 * copies the raw shape in (publish_shape) and notes the visibility (publish_visible); the control
 * thread reads a snapshot, and the conversion to BGRA32 happens there, once per shape.
 */
#pragma once

#include <cstdint>
#include <memory>

#include "cursor_shape.h"

namespace platf::cursor_bus {
  /// Capture thread: a new pointer shape. Every call starts a new shape generation, also when the
  /// shape is identical (a capture restart delivers the shape again).
  void publish_shape(cursor_shape::raw_t raw);

  /// Capture thread: whether the pointer is shown on the captured display. Only a change starts a
  /// new state generation.
  void publish_visible(bool visible);

  /// Whether the active capture backend reports pointer shapes (Desktop Duplication does,
  /// Windows.Graphics.Capture does not). Set by platf::display(); true until a backend says otherwise.
  void set_shapes_available(bool available);
  bool shapes_available();

  struct snapshot_t {
    std::uint64_t shape_gen = 0;  ///< 0 until the first shape was published
    std::uint64_t state_gen = 0;
    bool visible = true;
    /// The converted shape, or null while there is none or it is oversize or invalid
    std::shared_ptr<const cursor_shape::bgra_t> shape;
  };

  /// Control thread: the current shape and visibility. Converts the shape when its generation is
  /// new; later calls for the same generation return the same object.
  snapshot_t snapshot();

  /// The current converted shape when its id matches, else null (for /actions/cursor?shape=<id>).
  std::shared_ptr<const cursor_shape::bgra_t> find_shape(std::uint32_t id);

  /// Whether the capture draws the pointer into the video. False only while every streaming session
  /// asked for a local cursor; read per frame by the capture thread, written by the control thread
  /// and the HTTPS thread (stream.cpp).
  bool compose();
  void set_compose(bool on);
}  // namespace platf::cursor_bus
