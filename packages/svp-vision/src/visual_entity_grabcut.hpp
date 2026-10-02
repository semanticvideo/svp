#pragma once

#include <opencv2/core.hpp>

#include <cstdint>

namespace svp::vision::visual_entity_internal {

// cv::grabCut initializes its colour models with k-means++ (OpenCV
// grabcut.cpp initGMMs), which draws from cv::theRNG(): one generator per
// thread whose state advances with every earlier draw on that thread. Without
// a reseed, a mask would depend on every GrabCut call that ran before it on
// the same thread (earlier regions, earlier tracking windows), so a window
// computed on its own would not reproduce the window computed in sequence.
//
// The seed is cv::RNG's default state, the state every new thread's
// generator starts from. Reseeding to it before each call makes each mask a
// function of its image, box, and iteration count only, and keeps the first
// GrabCut call on a fresh thread identical to the unseeded behaviour.
inline constexpr std::uint64_t kGrabCutRngSeed = 0xffffffffULL;

// Runs cv::grabCut(GC_INIT_WITH_RECT) with the thread's generator reseeded to
// kGrabCutRngSeed, then restores the generator's previous state so that no
// other consumer on the thread observes GrabCut's draws. Exceptions from
// cv::grabCut propagate after the state is restored.
void run_seeded_grabcut(const cv::Mat& image,
                        cv::Mat& mask,
                        const cv::Rect& box,
                        int iterations);

}  // namespace svp::vision::visual_entity_internal
