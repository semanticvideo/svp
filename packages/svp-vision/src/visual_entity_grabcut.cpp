#include "visual_entity_grabcut.hpp"

#include <opencv2/imgproc.hpp>

namespace svp::vision::visual_entity_internal {
namespace {

class ThreadRngRestore {
 public:
  ThreadRngRestore() : saved_(cv::theRNG()) {}
  ~ThreadRngRestore() { cv::theRNG() = saved_; }
  ThreadRngRestore(const ThreadRngRestore&) = delete;
  ThreadRngRestore& operator=(const ThreadRngRestore&) = delete;

 private:
  cv::RNG saved_;
};

}  // namespace

void run_seeded_grabcut(const cv::Mat& image,
                        cv::Mat& mask,
                        const cv::Rect& box,
                        int iterations) {
  const ThreadRngRestore restore;
  cv::theRNG() = cv::RNG(kGrabCutRngSeed);
  cv::Mat background_model;
  cv::Mat foreground_model;
  cv::grabCut(image, mask, box, background_model, foreground_model, iterations,
              cv::GC_INIT_WITH_RECT);
}

}  // namespace svp::vision::visual_entity_internal
