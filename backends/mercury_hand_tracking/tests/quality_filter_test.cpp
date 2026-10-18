// SPDX-License-Identifier: BSL-1.0
#include "xr_mercury_quality_filter.hpp"
#include <cassert>
#include <iostream>
#include <limits>
#include <string>

int main(int argc, char** argv) {
    using namespace xr_mercury_quality;
    Config config;
    assert(!config.enabled);
    config.enabled = true;
    if (argc == 2 && std::string(argv[1]) == "--replay") {
        int a0, a1;
        double m0, m1, error;
        while (std::cin >> a0 >> a1 >> m0 >> m1 >> error) {
            const Sample sample{{a0 != 0, a1 != 0}, {m0, m1}};
            std::cout << int(reject_fit(config, sample, error)) << '\n';
        }
        return std::cin.eof() ? 0 : 2;
    }
    assert(argc == 1);
    const Sample strong{{true, true}, {0.6, 0.65}};
    assert(!reject_keypoints(config, strong));
    assert(!reject_fit(config, strong, 2.0));
    assert(reject_keypoints(config, Sample{{true, true}, {0.6, 0.14}}));
    assert(!reject_keypoints(config, Sample{{true, true}, {0.25, 0.6}}));
    assert(!reject_fit(config, Sample{{true, true}, {0.316, 0.351}}, 0.4));
    assert(reject_fit(config, Sample{{true, true}, {0.316, 0.351}}, 1.33));
    assert(!reject_fit(config, Sample{{true, true}, {0.35, 0.6}}, 0.51));
    assert(!reject_fit(config, Sample{{true, true}, {0.3, 0.6}}, 0.5));
    assert(!reject_fit(config, Sample{{true, false}, {0.6, 0.0}}, 0.4));
    assert(reject_keypoints(config, Sample{{false, false}, {0.6, 0.6}}));
    const double nan = std::numeric_limits<double>::quiet_NaN();
    assert(reject_keypoints(config, Sample{{true, true}, {nan, 0.6}}));
    assert(reject_fit(config, strong, nan));
    config.enabled = false;
    assert(!reject_fit(config, Sample{{true, true}, {0.01, 0.01}}, nan));
    unsetenv("MERCURY_XR_QUALITY_FILTER");
    unsetenv("MERCURY_XR_QUALITY_MIN_MEAN_XY");
    unsetenv("MERCURY_XR_QUALITY_WEAK_MEAN_XY");
    unsetenv("MERCURY_XR_QUALITY_MAX_WEAK_REPROJECTION");
    assert(!Config::from_environment().enabled);
    setenv("MERCURY_XR_QUALITY_FILTER", "", 1);
    assert(!Config::from_environment().enabled);
    for (const char* flag : {"0", "false", "no"}) {
        setenv("MERCURY_XR_QUALITY_FILTER", flag, 1);
        const Config disabled = Config::from_environment();
        assert(!disabled.enabled);
        assert(!reject_fit(disabled, Sample{{true, true}, {0.01, 0.01}}, nan));
    }
    for (const char* flag : {"1", "true", "yes"}) {
        setenv("MERCURY_XR_QUALITY_FILTER", flag, 1);
        assert(Config::from_environment().enabled);
    }
    setenv("MERCURY_XR_QUALITY_FILTER", "1", 1);
    setenv("MERCURY_XR_QUALITY_MIN_MEAN_XY", "0.3", 1);
    setenv("MERCURY_XR_QUALITY_WEAK_MEAN_XY", "0.4", 1);
    setenv("MERCURY_XR_QUALITY_MAX_WEAK_REPROJECTION", "0.75", 1);
    const Config custom = Config::from_environment();
    assert(custom.enabled && custom.min_mean_xy == 0.3 && custom.weak_mean_xy == 0.4 &&
           custom.max_weak_reprojection_error == 0.75);
    setenv("MERCURY_XR_QUALITY_FILTER", "0", 1);
    setenv("MERCURY_XR_QUALITY_MIN_MEAN_XY", "nan", 1);
    const Config invalid = Config::from_environment();
    assert(!invalid.enabled && invalid.min_mean_xy == 0.25);
}
