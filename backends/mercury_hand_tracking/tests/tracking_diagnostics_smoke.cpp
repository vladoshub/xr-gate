// SPDX-License-Identifier: BSL-1.0
#include "xr_mercury_tracking_diagnostics.hpp"
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const bool disabled = std::strcmp(argv[1], "disabled") == 0;
    if (disabled) unsetenv("MERCURY_XR_TRACKING_DEBUG_CSV");
    else setenv("MERCURY_XR_TRACKING_DEBUG_CSV", argv[1], 1);
    xr_mercury_diag::CsvLogger logger;
    logger.configure_from_environment();
    if (logger.enabled() == disabled) return 3;
    for (unsigned i = 0; i < 31; ++i) {
        logger.frame = {};
        logger.frame.timestamp_ns = 9007199254740993ULL + i * 33333333ULL;
        logger.frame.camera0_ns = logger.frame.timestamp_ns;
        logger.frame.camera1_ns = logger.frame.timestamp_ns + 1000;
        logger.frame.width = 480;
        logger.frame.height = 640;
        auto& hand = logger.frame.hands[0];
        hand.final_active = i < 30;
        hand.roi_dt_now_ms = 0;
        hand.roi_dt_past_ms = 33.333333;
        if (i < 30) {
            hand.optimizer_ran = 1;
            hand.palm_z = -0.2;
            hand.views[0].model_ran = 1;
            hand.views[0].is_hand_probability = 0.975;
            hand.views[0].confidence_xy_mean = 0.4;
        } else {
            hand.reject(xr_mercury_diag::no_roi);
            hand.reject(xr_mercury_diag::predicted_outside);
        }
        logger.frame.hands[1].final_active = 0;
        logger.write_frame();
    }
}
