#include "body_tracker_stability_filter.hpp"
#include "jitter_filter.hpp"

#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace xr_tracking;
using namespace xr_runtime_adapter;

static void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

static BodyTrackerF32V1 tracker(const char* id, float x) {
  BodyTrackerF32V1 out;
  std::snprintf(out.tracker_id, sizeof(out.tracker_id), "%s", id);
  out.status = BODY_TRACKER_STATUS_TRACKING;
  out.flags = BODY_TRACKER_FLAG_POSE_VALID | BODY_TRACKER_FLAG_CONNECTED;
  out.pose.px = x;
  return out;
}

int main() {
  try {
    constexpr uint64_t start_ns = 10'000'000'000;
    BodyTrackerSetFrameF32V1 input;
    input.sequence = 1;
    input.source_timestamp_ns = start_ns;
    input.tracker_count = 2;
    input.trackers[0] = tracker("left", 0.25f);
    input.trackers[1] = tracker("right", 0.75f);

    body_tracker_filter::BodyTrackerStabilityFilter stability;
    body_tracker_filter::BodyTrackerStabilityGateConfig stability_cfg;
    stability_cfg.enabled = true;
    stability.configure(stability_cfg);
    jitter_filter::BodyTrackerJitterFilter jitter;
    jitter_filter::RuntimeJitterFilterConfig jitter_cfg;
    jitter_cfg.enabled = true;
    jitter_cfg.tracker_position_smooth_alpha = 0.5;

    auto frame = stability.filter_observed(input, input.source_timestamp_ns);
    jitter.filter(frame, jitter_cfg);

    // The source omits the first tracker. The stability filter appends its held
    // pose after the observed tracker, reversing the order.
    input.source_timestamp_ns += 20'000'000;
    input.tracker_count = 1;
    input.trackers[0] = tracker("right", 0.875f);
    frame = stability.filter_observed(input, input.source_timestamp_ns);
    require(frame.tracker_count == 2 && std::string_view(frame.trackers[1].tracker_id) == "left",
            "Dropped tracker did not retain its own held pose");
    jitter.filter(frame, jitter_cfg);
    require(frame.trackers[0].pose.px == 0.8125f && frame.trackers[1].pose.px == 0.25f,
            "Changing the frame order mixed tracker histories");

    input.source_timestamp_ns += 10'000'000;
    input.tracker_count = 2;
    input.trackers[1] = tracker("left", 0.0f);
    input.trackers[1].status = BODY_TRACKER_STATUS_LOST;
    frame = stability.filter_observed(input, input.source_timestamp_ns);
    require(frame.trackers[1].status == BODY_TRACKER_STATUS_TRACKING && frame.trackers[1].pose.px == 0.25f,
            "Explicit LOST did not use the last tracked pose during the hold window");

    input.source_timestamp_ns = start_ns + 520'000'000;
    frame = stability.filter_observed(input, input.source_timestamp_ns);
    require(frame.trackers[1].status == BODY_TRACKER_STATUS_LOST,
            "Explicit LOST did not pass through when prediction expired");
    jitter.filter(frame, jitter_cfg);

    input.trackers[1] = tracker("left", 0.125f);
    frame = stability.filter_observed(input, ++input.source_timestamp_ns);
    jitter.filter(frame, jitter_cfg);
    require(frame.trackers[1].pose.px == 0.125f, "Reacquired tracker reused a lost pose");
    std::cout << "Body tracker filters: stable identity, reordering, hold and loss passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
