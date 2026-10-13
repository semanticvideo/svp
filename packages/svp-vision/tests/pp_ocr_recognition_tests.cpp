#include "../src/pp_ocr/pp_ocr_internal.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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

// One timestep per class index: a one-hot row for each, in order.
std::vector<float> one_hot_rows(const std::vector<int>& classes, int num_classes) {
  std::vector<float> rows(classes.size() * static_cast<std::size_t>(num_classes), 0.0f);
  for (std::size_t t = 0; t < classes.size(); ++t) {
    rows[t * static_cast<std::size_t>(num_classes) + static_cast<std::size_t>(classes[t])] =
        1.0f;
  }
  return rows;
}

std::string decode(const std::vector<int>& classes, int num_classes) {
  const std::vector<std::string> dict = {"a", "b"};  // classes: 0 blank, 1 a, 2 b, 3 space
  const auto rows = one_hot_rows(classes, num_classes);
  return svp::vision::pp_ocr_internal::ctc_decode(
      rows.data(), static_cast<int>(classes.size()), num_classes, dict);
}

void check_ctc_spaces() {
  check(decode({1, 3, 2}, 4) == "a b", "the class past the list is a space");
  check(decode({1, 1, 0, 1, 3, 3, 2}, 4) == "aa b", "repeats collapse, blanks separate");
  check(decode({3, 1, 3, 2, 3}, 4) == "a b", "spaces at either end are trimmed");
  check(decode({3, 0, 3}, 4).empty(), "only spaces decode to nothing");
  check(decode({1, 3, 2}, 5) == "ab", "a model with other extra classes adds no space");
}

void check_char_dict_quotes() {
  const auto path = std::filesystem::temp_directory_path() / "svp_pp_ocr_dict_test.yml";
  {
    std::ofstream out(path);
    out << "PostProcess:\n"
           "  character_dict:\n"
           "  - ''''\n"
           "  - '\"'\n"
           "  - \"b\"\n"
           "  - 'it''s'\n"
           "  - c\n"
           "Next: 1\n";
  }
  const auto dict = svp::vision::pp_ocr_internal::load_char_dict_from_yaml(path);
  std::filesystem::remove(path);
  check(dict == std::vector<std::string>{"'", "\"", "b", "it's", "c"},
        "single-quoted entries undo YAML's doubled apostrophe");
}

}  // namespace

int main() {
  using svp::vision::pp_ocr_internal::DetBox;
  using svp::vision::pp_ocr_internal::preprocess_recognition;

  check_ctc_spaces();
  check_char_dict_quotes();

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
