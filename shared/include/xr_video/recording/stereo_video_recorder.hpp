#pragma once

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include <xr_video/contracts/stereo_video_contract.hpp>

namespace xr_video {

struct StereoVideoRecorderConfig {
  std::string record_dir;
  std::string mic_device;
  uint32_t every_n = 1;
  uint64_t max_frames = 0;
  bool write_sbs = false;
};

struct StereoVideoRecorderStatus {
  bool active = false;
  std::string record_dir;
  std::string mic_device;
  uint64_t input_frames_seen = 0;
  uint64_t frames_written = 0;
  uint64_t frames_skipped = 0;
};

class StereoVideoRecorder {
 public:
  StereoVideoRecorder() = default;
  ~StereoVideoRecorder() { stop(); }

  StereoVideoRecorder(const StereoVideoRecorder&) = delete;
  StereoVideoRecorder& operator=(const StereoVideoRecorder&) = delete;

  bool active() const {
    std::lock_guard<std::mutex> lock(mu_);
    return active_;
  }

  StereoVideoRecorderStatus status() const {
    std::lock_guard<std::mutex> lock(mu_);
    StereoVideoRecorderStatus st;
    st.active = active_;
    st.record_dir = current_dir_.string();
    st.mic_device = cfg_.mic_device;
    st.input_frames_seen = input_frames_seen_;
    st.frames_written = frames_written_;
    st.frames_skipped = frames_skipped_;
    return st;
  }

  void start(StereoVideoRecorderConfig cfg) {
    std::lock_guard<std::mutex> lock(mu_);
    stop_locked();
    if (cfg.every_n == 0) cfg.every_n = 1;
    cfg_ = std::move(cfg);
    if (cfg_.record_dir.empty()) cfg_.record_dir = default_record_dir();

    current_dir_ = std::filesystem::path(cfg_.record_dir);
    std::filesystem::create_directories(current_dir_ / "cam0");
    std::filesystem::create_directories(current_dir_ / "cam1");
    if (cfg_.write_sbs) std::filesystem::create_directories(current_dir_ / "sbs");

    frames_csv_.open(current_dir_ / "frames.csv", std::ios::out | std::ios::trunc);
    if (!frames_csv_) {
      throw std::runtime_error("failed to open recorder frames.csv in " + current_dir_.string());
    }
    frames_csv_ << "record_index,frame_sequence,source_timestamp_ns,publish_timestamp_ns,"
                   "left_timestamp_ns,right_timestamp_ns,width,height,pixel_format,"
                   "left_file,right_file,sbs_file\n";

    input_frames_seen_ = 0;
    frames_written_ = 0;
    frames_skipped_ = 0;
    first_source_timestamp_ns_ = 0;
    last_source_timestamp_ns_ = 0;
    active_ = true;
    write_metadata_locked(false);
  }

  void stop() {
    std::lock_guard<std::mutex> lock(mu_);
    stop_locked();
  }

  void record(const StereoVideoFrame& frame) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!active_) return;

    ++input_frames_seen_;
    if (cfg_.every_n > 1 && ((input_frames_seen_ - 1) % cfg_.every_n) != 0) {
      ++frames_skipped_;
      return;
    }
    if (cfg_.max_frames > 0 && frames_written_ >= cfg_.max_frames) {
      ++frames_skipped_;
      return;
    }

    validate_frame(frame);
    if (frame.header.pixel_format != static_cast<uint32_t>(StereoVideoPixelFormat::Gray8)) {
      throw std::runtime_error("primitive stereo recorder currently supports GRAY8 only");
    }

    const uint64_t idx = frames_written_ + 1;
    const std::string stem = zero_pad(idx, 8);
    const std::string left_name = "cam0/" + stem + ".pgm";
    const std::string right_name = "cam1/" + stem + ".pgm";
    std::string sbs_name;

    write_pgm(current_dir_ / left_name, frame.header.width, frame.header.height, frame.left);
    write_pgm(current_dir_ / right_name, frame.header.width, frame.header.height, frame.right);
    if (cfg_.write_sbs) {
      sbs_name = "sbs/" + stem + ".pgm";
      write_sbs_pgm(current_dir_ / sbs_name, frame);
    }

    if (first_source_timestamp_ns_ == 0) first_source_timestamp_ns_ = frame.header.source_timestamp_ns;
    last_source_timestamp_ns_ = frame.header.source_timestamp_ns;

    frames_csv_ << idx << ','
                << frame.header.sequence << ','
                << frame.header.source_timestamp_ns << ','
                << frame.header.publish_timestamp_ns << ','
                << frame.header.left_timestamp_ns << ','
                << frame.header.right_timestamp_ns << ','
                << frame.header.width << ','
                << frame.header.height << ','
                << frame.header.pixel_format << ','
                << left_name << ','
                << right_name << ','
                << sbs_name << '\n';

    ++frames_written_;
    if ((frames_written_ % 30) == 0) frames_csv_.flush();
  }

 private:
  static std::string default_record_dir() {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm {};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << "xr_video_record_" << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return (std::filesystem::temp_directory_path() / os.str()).string();
  }

  static std::string zero_pad(uint64_t value, int width) {
    std::ostringstream os;
    os << std::setw(width) << std::setfill('0') << value;
    return os.str();
  }

  static void write_pgm(const std::filesystem::path& path,
                        uint32_t width,
                        uint32_t height,
                        const std::vector<uint8_t>& pixels) {
    if (pixels.size() != static_cast<size_t>(width) * static_cast<size_t>(height)) {
      throw std::runtime_error("PGM pixel size mismatch for " + path.string());
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("failed to open " + path.string());
    out << "P5\n" << width << ' ' << height << "\n255\n";
    out.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
  }

  static void write_sbs_pgm(const std::filesystem::path& path, const StereoVideoFrame& frame) {
    const uint32_t width = frame.header.width;
    const uint32_t height = frame.header.height;
    std::vector<uint8_t> sbs(static_cast<size_t>(width) * 2u * static_cast<size_t>(height));
    for (uint32_t y = 0; y < height; ++y) {
      const uint8_t* l = frame.left.data() + static_cast<size_t>(y) * width;
      const uint8_t* r = frame.right.data() + static_cast<size_t>(y) * width;
      uint8_t* dst = sbs.data() + static_cast<size_t>(y) * width * 2u;
      std::copy(l, l + width, dst);
      std::copy(r, r + width, dst + width);
    }
    write_pgm(path, width * 2u, height, sbs);
  }

  void write_metadata_locked(bool finalized) {
    nlohmann::json j = {
        {"format", "XR_STEREO_VIDEO_RECORDING_V1"},
        {"storage", "pgm_sequence"},
        {"pixel_format", "GRAY8"},
        {"layout", "separate_cam0_cam1"},
        {"record_dir", current_dir_.string()},
        {"mic_device", cfg_.mic_device},
        {"every_n", cfg_.every_n},
        {"max_frames", cfg_.max_frames},
        {"write_sbs", cfg_.write_sbs},
        {"frames_written", frames_written_},
        {"frames_skipped", frames_skipped_},
        {"first_source_timestamp_ns", first_source_timestamp_ns_},
        {"last_source_timestamp_ns", last_source_timestamp_ns_},
        {"finalized", finalized}};
    std::ofstream out(current_dir_ / "metadata.json", std::ios::out | std::ios::trunc);
    if (out) out << j.dump(2) << '\n';
  }

  void stop_locked() {
    if (!active_) return;
    active_ = false;
    if (frames_csv_) {
      frames_csv_.flush();
      frames_csv_.close();
    }
    write_metadata_locked(true);
  }

  mutable std::mutex mu_;
  StereoVideoRecorderConfig cfg_;
  bool active_ = false;
  std::filesystem::path current_dir_;
  std::ofstream frames_csv_;
  uint64_t input_frames_seen_ = 0;
  uint64_t frames_written_ = 0;
  uint64_t frames_skipped_ = 0;
  uint64_t first_source_timestamp_ns_ = 0;
  uint64_t last_source_timestamp_ns_ = 0;
};

}  // namespace xr_video
