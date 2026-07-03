#pragma once

#include <string>

namespace svp::vision {

struct InferencePerformanceOptions {
  std::string ocr_performance_profile = "background";
};

inline int recognition_workers_for_ocr_profile(const std::string& profile) {
  if (profile == "serial") return 1;
  if (profile == "background") return 2;
  if (profile == "fast") return 6;
  return 3;
}

}  // namespace svp::vision
