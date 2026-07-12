#include "../src/pp_ocr/pp_ocr_internal.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

int failure_count = 0;

void check(bool condition, std::string_view message) {
  if (condition) return;
  std::cerr << "FAILED: " << message << '\n';
  ++failure_count;
}

svp::vision::ColorRasterFrame make_frame(int width, int height) {
  svp::vision::ColorRasterFrame frame;
  frame.width = width;
  frame.height = height;
  frame.pixels.resize(static_cast<std::size_t>(width * height));
  return frame;
}

}  // namespace

int main() {
  using svp::vision::pp_ocr_internal::DetBox;
  using svp::vision::pp_ocr_internal::preprocess_recognition;

  const auto ordinary = make_frame(200, 100);
  const auto ordinary_input = preprocess_recognition(
      ordinary, DetBox{0, 0, 200, 100}, 48, 3200);
  check(ordinary_input.height == 48, "ordinary input height");
  check(ordinary_input.width == 320, "ordinary input width");
  check(ordinary_input.data.size() == 3u * 48u * 320u,
        "ordinary input allocation");

  const auto wide = make_frame(1200, 100);
  const auto wide_input = preprocess_recognition(
      wide, DetBox{0, 0, 1200, 100}, 48, 3200);
  check(wide_input.width == 576, "wide input width");

  const auto capped = make_frame(8000, 100);
  const auto capped_input = preprocess_recognition(
      capped, DetBox{0, 0, 8000, 100}, 48, 3200);
  check(capped_input.width == 3200, "capped input width");

  bool unaligned_width_rejected = false;
  try {
    (void)preprocess_recognition(
        ordinary, DetBox{0, 0, 200, 100}, 48, 321);
  } catch (const std::invalid_argument&) {
    unaligned_width_rejected = true;
  }
  check(unaligned_width_rejected, "unaligned maximum width is rejected");

  bool below_minimum_width_rejected = false;
  try {
    (void)preprocess_recognition(
        ordinary, DetBox{0, 0, 200, 100}, 48, 288);
  } catch (const std::invalid_argument&) {
    below_minimum_width_rejected = true;
  }
  check(below_minimum_width_rejected,
        "aligned maximum width below the minimum is rejected");

  return failure_count == 0 ? 0 : 1;
}
