#include "capture_service_cpp/interleaved_columns_decoder.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

}  // namespace

int main() {
  try {
    // Two rows, three pixels per eye, with two padding bytes per source row.
    const std::array<uint8_t, 16> source = {
        1, 101, 2, 102, 3, 103, 0xEE, 0xEF,
        4, 104, 5, 105, 6, 106, 0xFA, 0xFB,
    };
    std::array<uint8_t, 8> left{};
    std::array<uint8_t, 8> right{};

    xr_capture_cpp::deinterleave_gray8_columns(
        source.data(), source.size(), 8, 3, 2,
        left.data(), 4, right.data(), 4);

    require(left[0] == 1 && left[1] == 2 && left[2] == 3, "left row 0 mismatch");
    require(left[4] == 4 && left[5] == 5 && left[6] == 6, "left row 1 mismatch");
    require(right[0] == 101 && right[1] == 102 && right[2] == 103, "right row 0 mismatch");
    require(right[4] == 104 && right[5] == 105 && right[6] == 106, "right row 1 mismatch");

    bool rejected_short_buffer = false;
    try {
      xr_capture_cpp::deinterleave_gray8_columns(
          source.data(), 10, 8, 3, 2,
          left.data(), 4, right.data(), 4);
    } catch (const std::invalid_argument&) {
      rejected_short_buffer = true;
    }
    require(rejected_short_buffer, "short source buffer was not rejected");

    std::cout << "interleaved_columns_decoder_smoke: ok\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "interleaved_columns_decoder_smoke: " << error.what() << '\n';
    return 1;
  }
}
