#include "svp/media/canonical_raster.hpp"
#include "svp/media/canonical_timing.hpp"

#include <cassert>

namespace {

void test_round_half_to_even() {
  assert(svp::media::round_half_to_even(1, 2) == 0);
  assert(svp::media::round_half_to_even(3, 2) == 2);
  assert(svp::media::round_half_to_even(5, 2) == 2);
  assert(svp::media::round_half_to_even(7, 2) == 4);
}

void test_frame_rates() {
  assert(svp::media::frame_index_to_microseconds(1, {24000, 1001}) == 41708);
  assert(svp::media::frame_index_to_microseconds(1, {30000, 1001}) == 33367);
  assert(svp::media::frame_index_to_microseconds(1, {25, 1}) == 40000);
  assert(svp::media::frame_index_to_microseconds(1, {30, 1}) == 33333);
  assert(svp::media::frame_index_to_microseconds(2, {30, 1}) == 66667);
  assert(svp::media::frame_index_to_microseconds(1, {60, 1}) == 16667);
}

void test_canonical_rasters() {
  auto landscape =
      svp::media::compute_canonical_analysis_raster({1920, 1080, 0, {1, 1}});
  assert(landscape.width == 640);
  assert(landscape.height == 360);

  auto vertical =
      svp::media::compute_canonical_analysis_raster({1080, 1920, 0, {1, 1}});
  assert(vertical.width == 360);
  assert(vertical.height == 640);

  auto square =
      svp::media::compute_canonical_analysis_raster({1080, 1080, 0, {1, 1}});
  assert(square.width == 640);
  assert(square.height == 640);

  auto scope =
      svp::media::compute_canonical_analysis_raster({2048, 858, 0, {1, 1}});
  assert(scope.width == 640);
  assert(scope.height == 268);

  auto rotated =
      svp::media::compute_canonical_analysis_raster({1920, 1080, 90, {1, 1}});
  assert(rotated.width == 360);
  assert(rotated.height == 640);
}

}  // namespace

int main() {
  test_round_half_to_even();
  test_frame_rates();
  test_canonical_rasters();
  return 0;
}
