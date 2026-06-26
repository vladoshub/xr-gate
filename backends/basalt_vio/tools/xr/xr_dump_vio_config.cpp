#include <iostream>
#include <string>

#include <basalt/utils/vio_config.h>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: xr_dump_vio_config <out.json>\n";
    return 2;
  }

  basalt::VioConfig cfg;

  // Для XR live/offline smoke-test не хотим realtime drop policy.
  cfg.vio_enforce_realtime = false;

  // Это поле в твоём Basalt есть.
  cfg.vio_use_lm = true;

  try {
    cfg.save(argv[1]);
  } catch (const std::exception& e) {
    std::cerr << "failed to save VioConfig: " << e.what() << "\n";
    return 1;
  }

  std::cout << argv[1] << "\n";
  return 0;
}
