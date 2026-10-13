#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <ostream>
#include <sstream>
#include <string>

namespace xr_tracking::hand_pose_diagnostics {

inline bool enabled(const char* name) {
  const char* value = std::getenv(name);
  if (!value) return false;
  const std::string text(value);
  return text == "1" || text == "true" || text == "yes" || text == "on";
}

template <typename Side>
void write_side(std::ostream& out, const char* name, const Side& side) {
  const std::string p = std::string(" ") + name;
  out << p << "_status=" << side.status << p << "_flags=" << side.flags
      << p << "_handedness=" << side.handedness << p << "_confidence=" << side.confidence
      << p << "_controller_p=(" << side.controller_px << "," << side.controller_py << "," << side.controller_pz << ")"
      << p << "_controller_q=(" << side.controller_qx << "," << side.controller_qy << "," << side.controller_qz << "," << side.controller_qw << ")"
      << p << "_palm_p=(" << side.palm_px << "," << side.palm_py << "," << side.palm_pz << ")"
      << p << "_wrist_p=(" << side.wrist_px << "," << side.wrist_py << "," << side.wrist_pz << ")"
      << p << "_velocity=(" << side.vx << "," << side.vy << "," << side.vz << ")"
      << p << "_angular_velocity=(" << side.wx << "," << side.wy << "," << side.wz << ")"
      << p << "_joint_count=" << side.joint_count;
  const auto count = std::min<std::size_t>(side.joint_count, sizeof(side.joints) / sizeof(side.joints[0]));
  for (std::size_t i = 0; i < count; ++i) {
    const auto& joint = side.joints[i];
    if (joint.joint_id != 0 && joint.joint_id != 5 && joint.joint_id != 9 &&
        joint.joint_id != 13 && joint.joint_id != 17) continue;
    out << p << "_joint_" << joint.joint_id << "=(" << joint.px << "," << joint.py << ","
        << joint.pz << "," << joint.flags << "," << joint.confidence << ")";
  }
}

// Both the Mercury ABI-compatible publisher and runtime use this read-only
// formatter. No image buffers or pointers are written to the log.
template <typename Frame>
void write_frame(std::ostream& out, const char* stage, const Frame& frame,
                 uint64_t tick_ns, uint64_t input_sequence, uint64_t input_source_ns) {
  std::ostringstream line;
  line << "[hand_pose_diag] stage=" << stage << " tick_ns=" << tick_ns
       << " input_sequence=" << input_sequence << " input_source_ns=" << input_source_ns
       << " sequence=" << frame.sequence << " source_ns=" << frame.source_timestamp_ns
       << " publish_ns=" << frame.timestamp_ns << " reset_counter=" << frame.reset_counter
       << " tracking_status=" << frame.tracking_status << " flags=" << frame.flags
       << " hand_count=" << frame.hand_count;
  write_side(line, "left", frame.left);
  write_side(line, "right", frame.right);
  out << (line.str() + "\n");
}

}  // namespace xr_tracking::hand_pose_diagnostics
