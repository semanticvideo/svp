#include "../src/pp_ocr/pp_ocr_internal.hpp"

#include <cassert>

namespace {

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
  assert(ordinary_input.height == 48);
  assert(ordinary_input.width == 320);
  assert(ordinary_input.data.size() == 3u * 48u * 320u);

  const auto wide = make_frame(1200, 100);
  const auto wide_input = preprocess_recognition(
      wide, DetBox{0, 0, 1200, 100}, 48, 3200);
  assert(wide_input.width == 576);

  const auto capped = make_frame(8000, 100);
  const auto capped_input = preprocess_recognition(
      capped, DetBox{0, 0, 8000, 100}, 48, 3200);
  assert(capped_input.width == 3200);
}
