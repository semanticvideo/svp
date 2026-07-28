#include "svp/vision/visual_entity_detector.hpp"

#include <cassert>
#include <filesystem>

namespace {

void test_rejects_invalid_area_policy() {
  svp::vision::VisualEntityDetectorOptions options;
  options.minimum_area_ratio = options.maximum_area_ratio;
  const auto runtime = svp::vision::load_visual_entity_detector(
      std::filesystem::temp_directory_path() / "svp-missing-model-cache",
      options);
  assert(!runtime.session);
  assert(runtime.blocker == "invalid visual entity detector thresholds");
}

void test_rejects_zero_detection_cap() {
  svp::vision::VisualEntityDetectorOptions options;
  options.maximum_detections = 0;
  const auto runtime = svp::vision::load_visual_entity_detector(
      std::filesystem::temp_directory_path() / "svp-missing-model-cache",
      options);
  assert(!runtime.session);
  assert(runtime.blocker == "invalid visual entity detector thresholds");
}

}  // namespace

int main() {
  test_rejects_invalid_area_policy();
  test_rejects_zero_detection_cap();
  return 0;
}
