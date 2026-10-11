#pragma once

#include <initializer_list>

#include <xr_runtime/contracts/runtime_adapter.hpp>

namespace xr_runtime_adapter {

// A stream timeout is a new loss observation, not another optical sample.
// Keep the wire sequence for diagnostics, but advance the gate's loss clock.
template <class Frame>
void mark_hand_frame_unavailable(Frame& frame, uint64_t now_ns) {
  frame.source_timestamp_ns = now_ns;
  frame.tracking_status = 0;
  frame.flags = 0;
  frame.hand_count = 0;
  frame.confidence = 0;
  for (auto* side : {&frame.left, &frame.right}) {
    side->status = 0;
    side->flags = 0;
    side->confidence = 0;
    side->vx = side->vy = side->vz = 0;
    side->wx = side->wy = side->wz = 0;
  }
}

template <class Frame>
Frame held_hand_frame(Frame frame, uint64_t now_ns) {
  frame.timestamp_ns = frame.source_timestamp_ns = now_ns;
  frame.tracking_status = 2;
  for (auto* side : {&frame.left, &frame.right}) {
    if (side->status != 0) side->status = 2;
    side->flags &= ~(xr_runtime::HAND_LINEAR_VELOCITY_VALID |
                     xr_runtime::HAND_ANGULAR_VELOCITY_VALID);
    side->vx = side->vy = side->vz = 0;
    side->wx = side->wy = side->wz = 0;
  }
  return frame;
}

}  // namespace xr_runtime_adapter
