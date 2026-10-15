#include "capture_service_cpp/image_transform.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

cv::Mat expected_transform(const cv::Mat& input, int quarter_turns, int flip) {
  const bool swap = quarter_turns % 2 != 0;
  cv::Mat result(swap ? input.cols : input.rows, swap ? input.rows : input.cols, CV_8UC1);
  for (int y = 0; y < input.rows; ++y) for (int x = 0; x < input.cols; ++x) {
    int dx = x, dy = y;
    if (quarter_turns == 1) { dx = input.rows - 1 - y; dy = x; }
    else if (quarter_turns == 2) { dx = input.cols - 1 - x; dy = input.rows - 1 - y; }
    else if (quarter_turns == 3) { dx = y; dy = input.cols - 1 - x; }
    if (flip & 1) dx = result.cols - 1 - dx;
    if (flip & 2) dy = result.rows - 1 - dy;
    result.at<uint8_t>(dy, dx) = input.at<uint8_t>(y, x);
  }
  return result;
}
}

int main() {
  const std::vector<std::vector<std::string>> rotations{
      {"none", "0", ""}, {"cw90", "90", "clockwise"},
      {"rotate180", "180"}, {"ccw90", "270", "counterclockwise", "CCW90"}};
  const std::vector<std::vector<std::string>> flips{
      {"none", ""}, {"x", "horizontal", "h"},
      {"y", "vertical", "v"}, {"xy", "yx", "both", "XY"}};
  for (bool strided : {false, true}) {
    for (int r = 0; r < 4; ++r) for (int f = 0; f < 4; ++f) {
      for (const auto& rotation : rotations[r]) for (const auto& flip : flips[f]) {
        cv::Mat storage(7, 11, CV_8UC1);
        cv::Mat input = strided ? storage(cv::Rect(2, 1, 5, 4)) : cv::Mat(4, 5, CV_8UC1);
        for (int y = 0; y < input.rows; ++y) for (int x = 0; x < input.cols; ++x)
          input.at<uint8_t>(y,x) = static_cast<uint8_t>(y * 19 + x * 3);
        const cv::Mat original = input.clone();
        const cv::Mat expected = expected_transform(input,r,f);
        cv::Mat result = xr_capture_cpp::transform_gray_image(input,rotation,flip);
        require(result.size() == expected.size() && result.type() == CV_8UC1, "transform shape/type changed");
        require(cv::countNonZero(result != expected) == 0, "transform pixel coordinates changed");
        require(cv::countNonZero(input != original) == 0, "transform modified its source");
        require(result.isContinuous() && result.u != input.u, "result aliases the reusable source buffer");
        input.setTo(0);
        require(cv::countNonZero(result != expected) == 0, "later capture overwrote an earlier eye");
      }
    }
  }
  for (const auto& modes : {std::pair<std::string,std::string>{"invalid","none"}, {"none","invalid"}}) {
    bool rejected = false;
    try { xr_capture_cpp::transform_gray_image(cv::Mat(2,3,CV_8UC1),modes.first,modes.second); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected,"invalid transform accepted");
  }
  std::cout << "Image transform aliases, pixel coordinates, strided input and owned snapshots: OK\n";
}
