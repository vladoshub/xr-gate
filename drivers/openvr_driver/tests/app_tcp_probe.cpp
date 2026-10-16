#include "runtime_tcp_client.hpp"

#include <chrono>
#include <iostream>
#include <thread>
#include <nlohmann/json.hpp>

using namespace xr::openvr_driver;
using Json = nlohmann::json;

int main(int argc, char** argv) {
  try {
    if (argc != 3) throw std::runtime_error("usage: openvr_app_tcp_probe PORT DURATION_MS");
    RuntimePoseReaderConfig p; RuntimeHandReaderConfig h; RuntimeControllerStateReaderConfig c;
    p.tcp.port = h.tcp.port = c.tcp.port = static_cast<uint16_t>(std::stoi(argv[1]));
    p.tcp.reconnect_ms = h.tcp.reconnect_ms = c.tcp.reconnect_ms = 40;
    p.tcp.timeout_ms = h.tcp.timeout_ms = c.tcp.timeout_ms = 600;
#ifdef _WIN32
    auto pose = create_runtime_pose_reader(p);  // Windows auto must use TCP.
    auto hand_left = create_runtime_hand_reader(h), hand_right = create_runtime_hand_reader(h);
    auto left = create_runtime_controller_state_reader(c), right = create_runtime_controller_state_reader(c);
#else
    auto default_pose = create_runtime_pose_reader(p);
    auto default_hand = create_runtime_hand_reader(h);
    auto default_controller = create_runtime_controller_state_reader(c);
    if (std::string(default_pose->transport_name()).find("posix") == std::string::npos ||
        std::string(default_hand->transport_name()).find("posix") == std::string::npos ||
        std::string(default_controller->transport_name()).find("posix") == std::string::npos)
      throw std::runtime_error("Linux auto default changed");
    auto pose = create_tcp_runtime_pose_reader(p);
    auto hand_left = create_tcp_runtime_hand_reader(h), hand_right = create_tcp_runtime_hand_reader(h);
    auto left = create_tcp_runtime_controller_state_reader(c), right = create_tcp_runtime_controller_state_reader(c);
#endif
    if (std::string(pose->transport_name()) != "app_tcp") throw std::runtime_error("TCP reader not selected");
    const auto start = std::chrono::steady_clock::now();
    const auto duration = std::chrono::milliseconds(std::stoi(argv[2]));
    while (std::chrono::steady_clock::now() - start < duration) {
      RuntimePoseSample ps{}; RuntimeHandSample hs{}, hs2{};
      RuntimeControllerStateSample cs{}, cs2{};
      std::string error;
      const bool hp = pose->read_latest(ps, &error), hh = hand_left->read_latest(hs, nullptr);
      const bool hh2 = hand_right->read_latest(hs2, nullptr);
      const bool hc = left->read_latest(cs, nullptr), hc2 = right->read_latest(cs2, nullptr);
      Json out{{"ms", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count()},
               {"pose", hp}, {"hands", hh}, {"hands2", hh2}, {"controllers", hc}, {"controllers2", hc2},
               {"fresh", hp && ps.age_ns <= 100000000}, {"age_ns", ps.age_ns}, {"error", error},
               {"sequence", ps.pose.sequence}, {"position", {ps.pose.px, ps.pose.py, ps.pose.pz}},
               {"quaternion", {ps.pose.qx, ps.pose.qy, ps.pose.qz, ps.pose.qw}}, {"pose_flags", ps.pose.flags},
               {"left_buttons", std::to_string(cs.frame.left.buttons)}, {"right_buttons", std::to_string(cs2.frame.right.buttons)},
               {"source_hash", std::to_string(cs.frame.left.source_device_hash)}, {"press_counter", cs.frame.left.press_counters[0]},
               {"roles", {cs.frame.left.role, cs2.frame.right.role}},
               {"joints", hs.frame.left.joint_count}, {"right_joints", hs2.frame.right.joint_count},
               {"last_joint_x", hs.frame.left.joints[20].px}};
      std::cout << out.dump() << std::endl;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
