#include "../src/visual_entity_depth_proposals.hpp"

#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
  if (condition) return;
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

void test_coherent_foreground_is_one_proposal() {
  constexpr int width = 100;
  constexpr int height = 80;
  std::vector<std::uint16_t> depth(width * height, 1000);
  for (int y = 20; y < 60; ++y) {
    for (int x = 30; x < 70; ++x) {
      depth[static_cast<std::size_t>(y) * width + x] = 50000;
    }
  }
  const auto proposals =
      svp::vision::visual_entity_internal::propose_depth_regions(
          depth.data(), width, height, {});
  check(proposals.size() == 1,
        "one coherent foreground produces one proposal");
  if (!proposals.empty()) {
    check(proposals.front().bbox == cv::Rect(30, 20, 40, 40),
          "proposal bounds match the coherent foreground");
  }
}

void test_flat_depth_produces_no_proposals() {
  std::vector<std::uint16_t> depth(64 * 64, 12000);
  const auto proposals =
      svp::vision::visual_entity_internal::propose_depth_regions(
          depth.data(), 64, 64, {});
  check(proposals.empty(), "flat depth produces no proposals");
}

}  // namespace

int main() {
  test_coherent_foreground_is_one_proposal();
  test_flat_depth_produces_no_proposals();
  return failures == 0 ? 0 : 1;
}
