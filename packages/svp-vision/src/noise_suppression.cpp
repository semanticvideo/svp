#include "svp/vision/noise_suppression.hpp"

#include <atomic>

#include <opencv2/core/utils/logger.hpp>

namespace svp::vision {

namespace {
std::atomic<bool> g_opencv_verbose{false};
}

void set_opencv_verbose(bool verbose) {
  g_opencv_verbose.store(verbose, std::memory_order_relaxed);
  cv::utils::logging::setLogLevel(
      verbose
          ? cv::utils::logging::LOG_LEVEL_INFO
          : cv::utils::logging::LOG_LEVEL_ERROR);
}

bool opencv_verbose() {
  return g_opencv_verbose.load(std::memory_order_relaxed);
}

}  // namespace svp::vision
