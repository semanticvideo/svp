#pragma once

#include <cstdint>
#include <opencv2/core.hpp>
#include <vector>

namespace svp::vision::visual_entity_internal {

struct DepthRegionProposal {
  cv::Rect bbox;
  cv::Mat mask;
  double mean_depth = 0.0;
};

struct DepthRegionProposalOptions {
  int layer_count = 8;
  double minimum_area_ratio = 0.005;
  double maximum_area_ratio = 0.90;
  double adjacent_layer_merge_iom = 0.50;
};

[[nodiscard]] std::vector<DepthRegionProposal> propose_depth_regions(
    const std::uint16_t* depth_data,
    int width,
    int height,
    const DepthRegionProposalOptions& options);

}  // namespace svp::vision::visual_entity_internal
