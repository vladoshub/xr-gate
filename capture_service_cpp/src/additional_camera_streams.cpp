#include "capture_service_cpp/additional_camera_streams.hpp"

#include "capture_service_cpp/extra_capture.hpp"
#include "capture_service_cpp/image_transform.hpp"
#include "capture_service_cpp/platform/runtime_defaults.hpp"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

namespace xr_capture_cpp {
namespace {

constexpr const char* kLog = "[capture_service_cpp][additional_cameras]";

bool includes_mode(const RuntimeConfig& cfg, const char* mode) {
  return std::find(cfg.publish_modes.begin(), cfg.publish_modes.end(), mode) != cfg.publish_modes.end();
}

RuntimeConfig publisher_config(const StreamPublishers& primary) {
  const auto& main = primary.config();
  const auto& options = main.additional_camera_publish;
  RuntimeConfig cfg;
  cfg.config_path = main.config_path;
  cfg.profile_name = main.profile_name;
  cfg.namespace_name = options.namespace_name.empty() ? main.namespace_name + "_additional"
                                                      : options.namespace_name;
  cfg.registry_path = expand_user_path(options.registry_path);
  if (cfg.registry_path.empty()) {
    auto path = std::filesystem::path(main.registry_path);
    const auto stem = path.stem().string();
    cfg.registry_path = path.replace_filename(stem + "_additional.json").string();
  }
  cfg.publish_modes = options.publish_modes.value_or(main.publish_modes);
  if (cfg.publish_modes.empty() || std::any_of(cfg.publish_modes.begin(), cfg.publish_modes.end(),
      [](const std::string& mode) { return mode != "shm" && mode != "tcp"; })) {
    throw std::runtime_error("service.additional_cameras.publish must contain shm or tcp");
  }
  cfg.tcp_bind_host = options.tcp_bind_host;
  cfg.tcp_port = options.tcp_port;
  cfg.tcp_client_queue_size = options.tcp_client_queue_size;
  if (cfg.tcp_port <= 0 || cfg.tcp_port > 65535 || cfg.tcp_client_queue_size <= 0) {
    throw std::runtime_error("invalid service.additional_cameras TCP port or queue size");
  }
  finalize_platform_runtime_config(cfg);
  if (includes_mode(cfg, "tcp") && primary.tcp_enabled() && cfg.tcp_port == main.tcp_port) {
    throw std::runtime_error("additional camera TCP port overlaps the primary capture port");
  }
  return cfg;
}

bool valid_id(const std::string& value) {
  return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '_' || c == '-' || c == '.';
  });
}

struct Output {
  StreamSpec spec;
  ImageTransformConfig transform;
  bool enabled = true;
};

struct PendingFrame {
  cv::Mat image;
  uint64_t timestamp_ns = 0;
};

struct Source {
  std::vector<Output> outputs;
  int width = 0;
  int height = 0;
  PendingFrame incoming;
  PendingFrame pending;
  PendingFrame processing;
};

std::vector<std::string> protected_capture_paths(const RuntimeConfig& primary,
                                                 const std::vector<std::string>& primary_shm,
                                                 const std::optional<ExtraCaptureConfig>& extra) {
  std::vector<std::string> paths{primary.registry_path, primary.registry_path + ".tmp",
                                 primary.config_path, primary.backend_control_file, primary.extra_config_path};
  const auto main_devices = configured_camera_devices(primary.camera);
  paths.insert(paths.end(), main_devices.begin(), main_devices.end());
  if (primary.imu.enabled && primary.imu.driver == "serial") paths.push_back(primary.imu.serial.port);
  for (const auto& name : primary_shm) paths.push_back("/dev/shm/" + name);
  if (extra) {
    for (const auto& camera : extra->cameras) {
      const auto devices = configured_camera_devices(camera.camera);
      paths.insert(paths.end(), devices.begin(), devices.end());
    }
  }
  return paths;
}

void check_capture_path(const std::string& target, const std::vector<std::string>& protected_paths) {
  for (const auto& path : protected_paths) {
    if (!path.empty() && same_path(target, path)) {
      throw std::runtime_error("additional camera publisher path overlaps a capture resource: " + target);
    }
  }
}

}  // namespace

struct AdditionalCameraStreams::Impl {
  Impl(const RuntimeConfig& cfg, const ICameraSource& source, const StreamPublishers& primary)
      : publishers(publisher_config(primary)) {
    const auto& service = publishers.config();
    const auto primary_shm = primary.shm_names();
    std::optional<ExtraCaptureConfig> extra;
    std::error_code file_error;
    if (!cfg.extra_config_path.empty() && std::filesystem::is_regular_file(cfg.extra_config_path, file_error)) {
      try {
        extra = load_extra_capture_config(cfg.extra_config_path);
      } catch (const std::exception&) {
        // The extra process reports its own configuration errors.
      }
    }
    auto protected_paths = protected_capture_paths(cfg, primary_shm, extra);
    check_capture_path(service.registry_path, protected_paths);
    protected_paths.push_back(service.registry_path);
    check_capture_path(service.registry_path + ".tmp", protected_paths);
    protected_paths.push_back(service.registry_path + ".tmp");
    std::set<std::string> shm_names(primary_shm.begin(), primary_shm.end());
    std::vector<std::string> additional_shm;
    std::set<std::string> stream_ids;
    const auto views = source.views();
    const bool shm = includes_mode(service, "shm");
    for (const auto& [name, setting] : cfg.camera.additional_cameras) {
      try {
        const std::string id = resolve_camera_view_id(cfg.camera, views, setting.source);
        const auto view = std::find_if(views.begin(), views.end(), [&](const CameraViewSpec& spec) {
          return spec.source_id == id;
        });
        if (view == views.end()) {
          throw std::runtime_error(source.name() + " does not expose camera view " + setting.source);
        }
        if (!valid_id(setting.stream_id) || !valid_id(setting.frame_id) || (shm && setting.frame_id.size() >= 32)) {
          throw std::runtime_error("invalid stream or frame id");
        }
        if (setting.width < 0 || setting.height < 0 || setting.slot_count < 0) {
          throw std::runtime_error("width, height and slot_count must be non-negative");
        }
        if (view->width <= 0 || view->height <= 0) throw std::runtime_error("source view has invalid dimensions");
        const bool swapped = image_rotation_swaps_dimensions(setting.transform.rotate);
        // Validate the flip before advertising the stream.
        (void)transform_image(cv::Mat::zeros(1, 1, CV_8UC1), "none", setting.transform.flip);
        const int width = swapped ? view->height : view->width;
        const int height = swapped ? view->width : view->height;
        if ((setting.width && setting.width != width) || (setting.height && setting.height != height)) {
          throw std::runtime_error("expected output " + std::to_string(width) + "x" + std::to_string(height) +
                                   " from the configured view and transform");
        }
        const uint64_t bytes = static_cast<uint64_t>(width) * height;
        if (bytes > std::numeric_limits<int>::max()) throw std::runtime_error("camera view payload is too large");
        if (stream_ids.count(setting.stream_id)) throw std::runtime_error("duplicate additional stream id");
        if (shm) {
          const auto shm_name = sanitize_shm_name(service.namespace_name + "_" + setting.stream_id);
          check_capture_path("/dev/shm/" + shm_name, protected_paths);
          if (!shm_names.insert(shm_name).second) {
            throw std::runtime_error("duplicate or conflicting additional camera SHM name");
          }
          additional_shm.push_back(shm_name);
        }
        stream_ids.insert(setting.stream_id);
        const auto slots = static_cast<uint32_t>(setting.slot_count ? setting.slot_count
                                                                  : std::min(cfg.camera.slot_count, 8));
        StreamSpec spec{setting.stream_id, kKindImage, "IMAGE", static_cast<uint32_t>(width),
                        static_cast<uint32_t>(height), kFormatGray8, "GRAY8", static_cast<uint32_t>(bytes),
                        slots, setting.frame_id, "Independent view from the primary camera source"};
        auto& selected = sources[id];
        selected.width = view->width;
        selected.height = view->height;
        selected.outputs.push_back({std::move(spec), setting.transform});
      } catch (const std::exception& e) {
        std::cerr << kLog << "[" << name << "] " << e.what() << std::endl;
      }
    }
    if (sources.empty()) throw std::runtime_error("no valid additional camera outputs");
    if (extra) {
      check_extra_capture_resources(*extra, {service.registry_path, service.registry_path + ".tmp"},
                                    additional_shm, includes_mode(service, "tcp") ? service.tcp_port : 0);
    }
    size_t active_outputs = 0;
    for (auto& [id, entry] : sources) {
      for (auto& output : entry.outputs) {
        try {
          publishers.add_stream(output.spec, true);
          ++active_outputs;
        } catch (const std::exception& e) {
          output.enabled = false;
          std::cerr << kLog << "[" << output.spec.stream_id << "] " << e.what() << std::endl;
        }
      }
    }
    if (!active_outputs) throw std::runtime_error("could not start additional camera outputs");
    publishers.start();
    publishers.write_registry();
    thread = std::thread(&Impl::run, this);
  }

  ~Impl() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stop = true;
    }
    ready.notify_one();
    if (thread.joinable()) thread.join();
  }

  void run() {
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(mutex);
        ready.wait(lock, [&] { return stop || pending; });
        if (stop) return;
        pending = false;
        for (auto& [id, entry] : sources) {
          std::swap(entry.processing, entry.pending);
          entry.pending.timestamp_ns = 0;
        }
      }
      for (auto& [id, entry] : sources) {
        if (!entry.processing.timestamp_ns) continue;
        for (auto& output : entry.outputs) {
          if (!output.enabled) continue;
          try {
            const auto& spec = output.spec;
            const cv::Mat image = transform_image(entry.processing.image, output.transform.rotate, output.transform.flip);
            if (static_cast<uint32_t>(image.cols) != spec.width || static_cast<uint32_t>(image.rows) != spec.height) {
              throw std::runtime_error("decoded dimensions differ from the advertised view");
            }
            publishers.publish(spec.stream_id, image.ptr<uint8_t>(), image.total(), entry.processing.timestamp_ns,
                               spec.width, spec.height, spec.format_code, 0, spec.frame_id);
          } catch (const std::exception& e) {
            output.enabled = false;
            std::cerr << kLog << "[" << output.spec.stream_id << "] publication stopped: " << e.what() << std::endl;
          }
        }
      }
    }
  }

  StreamPublishers publishers;
  std::map<std::string, Source> sources;
  std::mutex mutex;
  std::condition_variable ready;
  bool incoming = false;
  bool pending = false;
  bool stop = false;
  std::thread thread;
};

AdditionalCameraStreams::AdditionalCameraStreams(const RuntimeConfig& cfg, const ICameraSource& source,
                                                 const StreamPublishers& primary)
    : impl_(std::make_unique<Impl>(cfg, source, primary)) {}

AdditionalCameraStreams::~AdditionalCameraStreams() = default;

const StreamPublishers& AdditionalCameraStreams::publishers() const { return impl_->publishers; }

bool AdditionalCameraStreams::wants(const std::string& source_id) const noexcept {
  return impl_->sources.count(source_id) != 0;
}

void AdditionalCameraStreams::submit(const std::string& source_id, const cv::Mat& image,
                                     uint64_t timestamp_ns) noexcept {
  auto& sources = impl_->sources;
  const auto source = sources.find(source_id);
  if (source == sources.end() || image.empty() || image.type() != CV_8UC1) return;
  if (image.cols != source->second.width || image.rows != source->second.height) return;
  auto& frame = source->second.incoming;
  try {
    image.copyTo(frame.image);
    frame.timestamp_ns = timestamp_ns ? timestamp_ns : steady_ns();
    impl_->incoming = true;
  } catch (const std::exception& e) {
    frame.timestamp_ns = 0;
    std::cerr << kLog << "[" << source_id << "] dropped view frame: " << e.what() << std::endl;
  }
}

bool AdditionalCameraStreams::finish_read() noexcept {
  if (!impl_->incoming) return false;
  impl_->incoming = false;
  std::unique_lock<std::mutex> lock(impl_->mutex, std::try_to_lock);
  for (auto& [id, entry] : impl_->sources) {
    if (!entry.incoming.timestamp_ns) continue;
    if (lock.owns_lock()) std::swap(entry.incoming, entry.pending);
    entry.incoming.timestamp_ns = 0;
  }
  if (lock.owns_lock()) {
    impl_->pending = true;
    lock.unlock();
    impl_->ready.notify_one();
  }
  return true;
}

}  // namespace xr_capture_cpp
