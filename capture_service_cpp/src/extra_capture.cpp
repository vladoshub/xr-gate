#include "capture_service_cpp/extra_capture.hpp"

#include "capture_service_cpp/config/capture_config.hpp"
#include "capture_service_cpp/image_transform.hpp"
#include "capture_service_cpp/platform/camera_capture.hpp"
#include "capture_service_cpp/sources/camera_source.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <thread>

#include <capture_client/contracts/messages.hpp>

namespace xr_capture_cpp {
namespace {

constexpr auto kBgr8 = static_cast<uint32_t>(capture_client::FormatCode::BGR8);

bool publishes(const RuntimeConfig& cfg, const char* mode) {
  return std::find(cfg.publish_modes.begin(), cfg.publish_modes.end(), mode) != cfg.publish_modes.end();
}

void protect_file(const std::string& path, const std::string& temporary, const std::string& protected_path) {
  if (same_path(path, protected_path) || same_path(temporary, protected_path)) {
    throw std::runtime_error("extra registry overlaps an existing capture path: " + protected_path);
  }
}

bool valid_id(const std::string& value) {
  return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '_' || c == '-' || c == '.';
  });
}

struct CameraWorker {
  std::string name;
  RuntimeConfig config;
  std::vector<StreamSpec> streams;
};

CameraWorker prepare_camera(const RuntimeConfig& service, const ExtraCameraConfig& camera) {
  CameraWorker worker{camera.name, service, {}};
  worker.config.camera = camera.camera;
  const auto width = static_cast<uint32_t>(camera.camera.output_width);
  const auto height = static_cast<uint32_t>(camera.camera.output_height);
  const bool color = camera.format == "bgr8";
  const uint64_t bytes = static_cast<uint64_t>(width) * height * (color ? 3 : 1);
  // Both the capture wire protocol and the existing stereo spec builders use
  // fixed-width sizes. Check the product before building any stream specs.
  if (bytes > std::numeric_limits<int>::max()) throw std::runtime_error("camera output is too large");
  const auto slots = static_cast<uint32_t>(camera.camera.slot_count);
  if (camera.camera.layout == "mono") {
    worker.streams.push_back({camera.stream_id, kKindImage, "IMAGE", width, height,
                              color ? kBgr8 : kFormatGray8, color ? "BGR8" : "GRAY8",
                              static_cast<uint32_t>(bytes), slots, camera.frame_id,
                              "Independent camera frame"});
  } else {
    worker.streams = create_camera_source(worker.config, nullptr)->stream_specs(slots);
  }
  return worker;
}

void publish_image(StreamPublishers& publishers, const StreamSpec& spec, const cv::Mat& image,
                   uint64_t timestamp_ns) {
  publishers.publish(spec.stream_id, image.ptr<uint8_t>(), image.total() * image.elemSize(), timestamp_ns,
                     static_cast<uint32_t>(image.cols), static_cast<uint32_t>(image.rows),
                     spec.format_code, 0, spec.frame_id);
}

template <typename ReadAndPublish>
void capture_frames(int stall_ms, ReadAndPublish read_and_publish) {
  uint64_t last_frame_ns = steady_ns();
  while (!g_stop.load()) {
    const auto status = read_and_publish();
    if (status == SourceReadStatus::Data) {
      last_frame_ns = steady_ns();
      continue;
    }
    if (status == SourceReadStatus::EndOfStream ||
        (stall_ms > 0 && steady_ns() - last_frame_ns >= static_cast<uint64_t>(stall_ms) * 1000000ULL)) {
      throw std::runtime_error("camera stopped delivering frames");
    }
    if (status == SourceReadStatus::Timeout) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

void capture_mono(const CameraWorker& worker, StreamPublishers& publishers) {
  const auto& cfg = worker.config.camera;
  const auto& spec = worker.streams.front();
  CameraCapture camera;
  if (!camera.open(cfg.primary, worker.name)) throw std::runtime_error("failed to open camera");
  capture_frames(cfg.stall_exit_ms, [&] {
    cv::Mat frame;
    if (!camera.read(frame) || frame.empty()) return SourceReadStatus::Timeout;
    const uint64_t timestamp_ns = steady_ns();
    const int channels = frame.channels();
    if (frame.depth() != CV_8U || (channels != 1 && channels != 3 && channels != 4)) {
      throw std::runtime_error("expected decoded 8-bit grayscale, BGR or BGRA frames");
    }
    if (spec.format_code == kFormatGray8 && channels != 1) {
      frame = to_gray8(frame);
    } else if (spec.format_code == kBgr8 && channels != 3) {
      cv::Mat bgr;
      cv::cvtColor(frame, bgr, channels == 1 ? cv::COLOR_GRAY2BGR : cv::COLOR_BGRA2BGR);
      frame = std::move(bgr);
    }
    frame = transform_image(frame, cfg.left_transform.rotate, cfg.left_transform.flip);
    if (frame.cols != cfg.output_width || frame.rows != cfg.output_height) {
      throw std::runtime_error("output dimensions are " + std::to_string(frame.cols) + "x" +
                               std::to_string(frame.rows) + ", configured " +
                               std::to_string(cfg.output_width) + "x" + std::to_string(cfg.output_height));
    }
    publish_image(publishers, spec, frame, timestamp_ns);
    return SourceReadStatus::Data;
  });
}

void capture_stereo(const CameraWorker& worker, StreamPublishers& publishers) {
  auto camera = create_camera_source(worker.config, nullptr);
  camera->open();
  capture_frames(worker.config.camera.stall_exit_ms, [&] {
    StereoFrame frame;
    const auto status = camera->read(frame);
    if (!frame.complete()) {
      return status == SourceReadStatus::Data ? SourceReadStatus::TransportActivity : status;
    }
    const uint64_t timestamp_ns = frame.timestamp_ns ? frame.timestamp_ns : steady_ns();
    publish_image(publishers, worker.streams[0], frame.left, timestamp_ns);
    publish_image(publishers, worker.streams[1], frame.right, timestamp_ns);
    return SourceReadStatus::Data;
  });
  camera->close();
}

void run_camera(const CameraWorker& worker, StreamPublishers& publishers) {
  while (!g_stop.load()) {
    try {
      if (worker.config.camera.layout == "mono") capture_mono(worker, publishers);
      else capture_stereo(worker, publishers);
    } catch (const std::exception& e) {
      std::cerr << "[capture_service_cpp][extra][" << worker.name << "] " << e.what()
                << "; reopening in 2 s" << std::endl;
    }
    for (int i = 0; i < 20 && !g_stop.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

}  // namespace

void start_extra_capture(ExtraCaptureProcess& process, const RuntimeConfig& primary,
                         const StreamPublishers& publishers, const StreamPublishers* additional) {
  std::vector<std::string> args{kExtraCaptureWorkerArg, primary.extra_config_path,
                                std::to_string(current_process_id())};
  auto reserve = [&](const char* type, const std::string& value) {
    args.emplace_back(type);
    args.push_back(value);
  };
  auto reserve_publishers = [&](const StreamPublishers& group) {
    const auto& cfg = group.config();
    reserve("--file", cfg.registry_path);
    reserve("--file", cfg.registry_path + ".tmp");
    if (group.tcp_enabled()) reserve("--tcp-port", std::to_string(cfg.tcp_port));
    for (const auto& name : group.shm_names()) {
      reserve("--shm", name);
      reserve("--file", "/dev/shm/" + name);
    }
  };
  reserve_publishers(publishers);
  if (additional) reserve_publishers(*additional);
  if (!primary.config_path.empty() && primary.config_path.front() != '<') reserve("--file", primary.config_path);
  if (!primary.backend_control_file.empty()) reserve("--file", primary.backend_control_file);
  if (primary.imu.enabled && primary.imu.driver == "serial" && !primary.imu.serial.port.empty()) {
    reserve("--file", primary.imu.serial.port);
  }
  for (const auto& device : configured_camera_devices(primary.camera)) reserve("--device", device);
  process.start(std::move(args));
}

void check_extra_capture_resources(const ExtraCaptureConfig& extra,
                                   const std::vector<std::string>& protected_files,
                                   const std::vector<std::string>& protected_shm,
                                   int reserved_tcp_port) {
  const auto& cfg = extra.service;
  if (publishes(cfg, "tcp") && cfg.tcp_port == reserved_tcp_port) {
    throw std::runtime_error("additional camera TCP port overlaps the extra capture port");
  }
  for (const auto& path : protected_files) protect_file(cfg.registry_path, cfg.registry_path + ".tmp", path);
  for (const auto& name : protected_shm) {
    protect_file(cfg.registry_path, cfg.registry_path + ".tmp", "/dev/shm/" + name);
  }
  if (!publishes(cfg, "shm")) return;
  for (const auto& camera : extra.cameras) {
    if (!camera.camera.enabled) continue;
    const auto worker = prepare_camera(cfg, camera);
    for (const auto& spec : worker.streams) {
      const auto name = sanitize_shm_name(cfg.namespace_name + "_" + spec.stream_id);
      if (std::find(protected_shm.begin(), protected_shm.end(), name) != protected_shm.end()) {
        throw std::runtime_error("additional camera SHM name overlaps extra capture: " + name);
      }
      for (const auto& path : protected_files) {
        if (same_path("/dev/shm/" + name, path)) {
          throw std::runtime_error("additional camera registry overlaps extra capture SHM: " + path);
        }
      }
    }
  }
}

int run_extra_capture(int argc, char** argv) {
  if (argc < 4 || argc % 2 != 0) throw std::runtime_error("invalid extra capture worker arguments");
  bind_extra_capture_to_parent(std::stoll(argv[3]));
  ExtraCaptureConfig extra = load_extra_capture_config(argv[2]);
  const auto& service = extra.service;
  const bool shm = publishes(service, "shm");
  const std::string registry_tmp = service.registry_path + ".tmp";
  std::vector<std::string> devices;
  std::vector<std::string> protected_files{service.config_path, service.registry_path, registry_tmp};
  std::set<std::string> shm_names;
  for (int i = 4; i < argc; i += 2) {
    const std::string option = argv[i];
    const std::string value = argv[i + 1];
    if (option == "--device") {
      protect_file(service.registry_path, registry_tmp, value);
      devices.push_back(value);
    } else if (option == "--shm") {
      shm_names.insert(value);
    } else if (option == "--file") {
      protect_file(service.registry_path, registry_tmp, value);
      protected_files.push_back(value);
    } else if (option == "--tcp-port" && publishes(service, "tcp") && service.tcp_port == std::stoi(value)) {
      throw std::runtime_error("extra TCP port must differ from the primary capture TCP port and additional camera TCP port");
    }
  }
  protect_file(service.registry_path, registry_tmp, service.config_path);
  if (same_path(service.registry_path, registry_tmp)) {
    throw std::runtime_error("extra registry and its temporary file overlap");
  }

  std::vector<CameraWorker> workers;
  std::set<std::string> stream_ids;
  for (const auto& camera : extra.cameras) {
    if (!camera.camera.enabled) continue;
    const auto inputs = configured_camera_devices(camera.camera);
    for (const auto& input : inputs) protect_file(service.registry_path, registry_tmp, input);
    const auto overlap = std::find_if(inputs.begin(), inputs.end(), [&](const std::string& input) {
      return std::any_of(devices.begin(), devices.end(), [&](const std::string& used) {
        return same_path(input, used);
      });
    });
    if (overlap != inputs.end() || (inputs.size() == 2 && same_path(inputs[0], inputs[1]))) {
      std::cerr << "[capture_service_cpp][extra][" << camera.name
                << "] camera device is already assigned: "
                << (overlap == inputs.end() ? inputs.front() : *overlap) << std::endl;
      continue;
    }
    CameraWorker worker = prepare_camera(service, camera);
    for (const auto& spec : worker.streams) {
      if (!valid_id(spec.stream_id) || !valid_id(spec.frame_id) || (shm && spec.frame_id.size() >= 32)) {
        throw std::runtime_error("invalid extra stream or frame id for " + camera.name);
      }
      if (!stream_ids.insert(spec.stream_id).second) {
        throw std::runtime_error("duplicate extra stream id: " + spec.stream_id);
      }
      if (shm) {
        const std::string name = sanitize_shm_name(service.namespace_name + "_" + spec.stream_id);
        if (!shm_names.insert(name).second) {
          throw std::runtime_error("extra SHM name collides for stream: " + spec.stream_id);
        }
        for (const auto& path : protected_files) {
          if (same_path("/dev/shm/" + name, path)) {
            throw std::runtime_error("extra SHM object overlaps a capture path: " + path);
          }
        }
      }
    }
    devices.insert(devices.end(), inputs.begin(), inputs.end());
    workers.push_back(std::move(worker));
  }
  if (workers.empty()) return kExitOk;

  StreamPublishers publishers(service);
  for (const auto& worker : workers) {
    for (const auto& spec : worker.streams) publishers.add_stream(spec);
  }
  publishers.start();
  publishers.write_registry();
  std::vector<std::thread> threads;
  threads.reserve(workers.size());
  try {
    for (const auto& worker : workers) threads.emplace_back(run_camera, std::cref(worker), std::ref(publishers));
  } catch (...) {
    g_stop.store(true);
    for (auto& thread : threads) thread.join();
    throw;
  }
  for (auto& thread : threads) thread.join();
  return kExitOk;
}

}  // namespace xr_capture_cpp
