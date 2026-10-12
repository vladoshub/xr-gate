// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace xr_mercury_quality {

struct Config {
    bool enabled = true;
    double min_mean_xy = 0.25;
    double weak_mean_xy = 0.35;
    double max_weak_reprojection_error = 0.5;

    static Config from_environment() {
        Config result;
        const char* flag = std::getenv("MERCURY_XR_QUALITY_FILTER");
        if (flag && flag[0]) {
            result.enabled = std::strcmp(flag, "1") == 0 ||
                             std::strcmp(flag, "true") == 0 ||
                             std::strcmp(flag, "yes") == 0;
        }
        read_number("MERCURY_XR_QUALITY_MIN_MEAN_XY", result.min_mean_xy, 1.0);
        read_number("MERCURY_XR_QUALITY_WEAK_MEAN_XY", result.weak_mean_xy, 1.0);
        read_number("MERCURY_XR_QUALITY_MAX_WEAK_REPROJECTION", result.max_weak_reprojection_error,
                    std::numeric_limits<double>::max());
        if (result.weak_mean_xy < result.min_mean_xy) result.weak_mean_xy = result.min_mean_xy;
        return result;
    }

private:
    static void read_number(const char* name, double& output, double upper) {
        const char* value = std::getenv(name);
        if (!value || !value[0]) return;
        char* end = nullptr;
        const double parsed = std::strtod(value, &end);
        if (end == value || *end != '\0' || !std::isfinite(parsed) || parsed < 0 || parsed > upper) {
            std::fprintf(stderr, "[mercury_quality_filter] ignoring invalid %s\n", name);
            return;
        }
        output = parsed;
    }
};

struct Sample {
    std::array<bool, 2> active{false, false};
    std::array<double, 2> mean_xy{0.0, 0.0};
};

inline bool reject_keypoints(const Config& config, const Sample& sample) {
    if (!config.enabled) return false;
    bool any_active = false;
    for (int view = 0; view < 2; ++view) {
        if (!sample.active[view]) continue;
        any_active = true;
        if (!std::isfinite(sample.mean_xy[view]) || sample.mean_xy[view] < config.min_mean_xy)
            return true;
    }
    return !any_active;
}

inline bool reject_fit(const Config& config, const Sample& sample, double reprojection_error) {
    if (!config.enabled) return false;
    if (reject_keypoints(config, sample) || !std::isfinite(reprojection_error)) return true;
    bool weak = false;
    for (int view = 0; view < 2; ++view) {
        if (sample.active[view] && sample.mean_xy[view] < config.weak_mean_xy) weak = true;
    }
    return weak && reprojection_error > config.max_weak_reprojection_error;
}

} // namespace xr_mercury_quality
