#include "hmd_pose_stability_filter.hpp"
#include <iostream>
#include <stdexcept>

using namespace xr_runtime_adapter::hmd_filter;

static void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

static xr_runtime::HmdPoseF64V1 pose(uint64_t seq, uint64_t stamp, double x, uint64_t epoch = 1) {
  xr_runtime::HmdPoseF64V1 p{};
  p.sequence = seq; p.timestamp_ns = p.source_timestamp_ns = stamp;
  p.reset_counter = epoch; p.px = x; p.qw = 1;
  p.tracking_status = 2; p.flags = xr_runtime::HMD_FLAG_POSE_VALID; p.confidence = 1;
  return p;
}

int main() {
  try {
    HmdPoseStabilityFilter filter;
    HmdPoseStabilityFilterConfig config; config.enabled = true;
    filter.configure(config);
    filter.filter(pose(1, 1000000000, 0), 1020000000);
    require(filter.has_good_pose(), "First accepted pose did not initialize the filter");
    HmdPoseStabilityFilterDecision decision;
    auto out = filter.filter(pose(2, 1033333333, 10), 1053333333, &decision);
    require(decision.used_last_good_pose && out.px == 0 && filter.rejected_count() == 1,
            "Ten metre jump escaped the enabled filter");

    filter.reset();
    filter.filter(pose(1, 1000000000, 0), 1200000000);
    // Poll the same sample several times immediately before a new source pose.
    filter.filter(pose(1, 1000000000, 0), 1233000000);
    out = filter.filter(pose(2, 1033333333, .033), 1233333333, &decision);
    require(!decision.output_substituted && out.px == .033,
            "Polling duplicates or receive-time jitter rejected valid 1 m/s motion");
    out = filter.filter(pose(3, 1066666666, .066), 1234333333, &decision);
    require(!decision.output_substituted && out.px == .066,
            "Batched source poses were timed in the host receive clock");

    out = filter.filter(pose(1, 2000000000, 50, 2), 2020000000, &decision);
    require(!decision.output_substituted && out.px == 50,
            "An explicit source reset retained the previous world anchor");
    std::cout << "HMD stability: first pose, jumps, duplicates, receive jitter and resets passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
