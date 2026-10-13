// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

// Diagnostics only: no tracking thresholds or decisions are changed here.
namespace xr_mercury_diag {

enum RejectReason : unsigned {
    no_roi = 1,
    overlap = 2,
    keypoint_rejected = 4,
    reprojection_rejected = 8,
    distance_rejected = 16,
    predicted_outside = 32,
    display_warmup = 64,
    quality_rejected = 128,
};

#define XR_MERCURY_DIAG_HAND_FIELDS(X) \
    X(previous_detected) X(history_frames) X(tracked_frames_before) \
    X(detector_ran) X(optimizer_ran) X(observation_views) \
    X(reprojection_error) X(reprojection_limit) X(smoothing_factor) X(depth_weight) \
    X(optimize_hand_size) X(target_hand_size) X(result_hand_size) \
    X(palm_x) X(palm_y) X(palm_z) X(palm_qx) X(palm_qy) X(palm_qz) X(palm_qw) \
    X(wrist_x) X(wrist_y) X(wrist_z) \
    X(roi_dt_past_ms) X(roi_dt_now_ms) X(roi_prediction_multiplier) \
    X(predicted_wrist_x) X(predicted_wrist_y) X(predicted_wrist_z) \
    X(outside_limit) X(overlap_limit) X(tracked_before_roi_prediction) \
    X(final_active) X(rejection_mask)

#define XR_MERCURY_DIAG_VIEW_FIELDS(X) \
    X(roi_before_overlap_found) X(roi_input_found) X(roi_provenance) \
    X(roi_cx) X(roi_cy) X(roi_size) \
    X(model_ran) X(keypoint_active) X(is_hand_probability) \
    X(confidence_xy_mean) X(confidence_xy_min) \
    X(confidence_depth_mean) X(confidence_depth_min) \
    X(overlap_iou_used) X(overlap_iou_correct_size) \
    X(roi_next_found) X(predicted_outside_joints) \
    X(roi_next_cx) X(roi_next_cy) X(roi_next_size)

struct View {
#define XR_FIELD(name) double name = std::numeric_limits<double>::quiet_NaN();
    XR_MERCURY_DIAG_VIEW_FIELDS(XR_FIELD)
#undef XR_FIELD
    View() { model_ran = 0; }
};

struct Hand {
#define XR_FIELD(name) double name = std::numeric_limits<double>::quiet_NaN();
    XR_MERCURY_DIAG_HAND_FIELDS(XR_FIELD)
#undef XR_FIELD
    std::array<View, 2> views{};
    Hand() { detector_ran = optimizer_ran = rejection_mask = 0; }
    void reject(RejectReason reason) {
        rejection_mask = static_cast<unsigned>(rejection_mask) | reason;
    }
};

struct Frame {
    std::uint64_t timestamp_ns = 0;
    std::uint64_t camera0_ns = 0;
    std::uint64_t camera1_ns = 0;
    unsigned width = 0;
    unsigned height = 0;
    std::array<Hand, 2> hands{};
};

class CsvLogger {
public:
    Frame frame{};
    CsvLogger() = default;
    CsvLogger(const CsvLogger&) = delete;
    CsvLogger& operator=(const CsvLogger&) = delete;
    ~CsvLogger() { if (file_) std::fclose(file_); }

    bool enabled() const { return file_ != nullptr; }

    void configure_from_environment() {
        const char* path = std::getenv("MERCURY_XR_TRACKING_DEBUG_CSV");
        if (!path || !path[0]) return;
        file_ = std::fopen(path, "w");
        if (!file_) {
            std::fprintf(stderr, "[mercury_tracking_diag] cannot open CSV: %s\n", path);
            return;
        }
        std::fputs("schema_version,frame,timestamp_ns,camera0_ns,camera1_ns,width,height,hand_idx", file_);
#define XR_HEADER(name) std::fputs("," #name, file_);
        XR_MERCURY_DIAG_HAND_FIELDS(XR_HEADER)
#undef XR_HEADER
        for (int view = 0; view < 2; ++view) {
#define XR_HEADER(name) std::fprintf(file_, ",v%d_" #name, view);
            XR_MERCURY_DIAG_VIEW_FIELDS(XR_HEADER)
#undef XR_HEADER
        }
        std::fputc('\n', file_);
        std::fflush(file_);
        std::fprintf(stderr, "[mercury_tracking_diag] enabled schema=1 csv=%s\n", path);
    }

    void write_frame() {
        if (!file_) return;
        ++frame_count_;
        for (int hand_idx = 0; hand_idx < 2; ++hand_idx) {
            const Hand& hand = frame.hands[hand_idx];
            std::fprintf(file_, "1,%llu,%llu,%llu,%llu,%u,%u,%d",
                         static_cast<unsigned long long>(frame_count_),
                         static_cast<unsigned long long>(frame.timestamp_ns),
                         static_cast<unsigned long long>(frame.camera0_ns),
                         static_cast<unsigned long long>(frame.camera1_ns),
                         frame.width, frame.height, hand_idx);
#define XR_VALUE(name) std::fprintf(file_, ",%.9g", hand.name);
            XR_MERCURY_DIAG_HAND_FIELDS(XR_VALUE)
#undef XR_VALUE
            for (const View& view : hand.views) {
#define XR_VALUE(name) std::fprintf(file_, ",%.9g", view.name);
                XR_MERCURY_DIAG_VIEW_FIELDS(XR_VALUE)
#undef XR_VALUE
            }
            std::fputc('\n', file_);
        }
        // Buffered output; preserve both hands together once per second at 30 Hz.
        if (frame_count_ % 30 == 0) std::fflush(file_);
        if (std::ferror(file_)) {
            std::fprintf(stderr, "[mercury_tracking_diag] CSV write failed; logging disabled\n");
            std::fclose(file_);
            file_ = nullptr;
        }
    }

private:
    std::FILE* file_ = nullptr;
    std::uint64_t frame_count_ = 0;
};

#undef XR_MERCURY_DIAG_HAND_FIELDS
#undef XR_MERCURY_DIAG_VIEW_FIELDS
} // namespace xr_mercury_diag
