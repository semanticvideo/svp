#include "visual_entity_depth_proposals.hpp"

#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>
#include <stdexcept>

namespace svp::vision::visual_entity_internal {
namespace {

double intersection_over_minimum(const cv::Rect& left, const cv::Rect& right) {
  const cv::Rect intersection = left & right;
  if (intersection.empty()) return 0.0;
  const double minimum_area =
      static_cast<double>(std::min(left.area(), right.area()));
  return minimum_area > 0.0
      ? static_cast<double>(intersection.area()) / minimum_area
      : 0.0;
}

void merge_adjacent_proposals(
    std::vector<DepthRegionProposal>& proposals,
    double minimum_iom) {
  bool merged = true;
  while (merged) {
    merged = false;
    for (std::size_t left = 0; left < proposals.size() && !merged; ++left) {
      for (std::size_t right = left + 1; right < proposals.size(); ++right) {
        if (intersection_over_minimum(
                proposals[left].bbox, proposals[right].bbox) < minimum_iom) {
          continue;
        }
        proposals[left].bbox |= proposals[right].bbox;
        cv::bitwise_or(
            proposals[left].mask, proposals[right].mask, proposals[left].mask);
        proposals[left].mean_depth =
            (proposals[left].mean_depth + proposals[right].mean_depth) / 2.0;
        proposals.erase(proposals.begin() + static_cast<std::ptrdiff_t>(right));
        merged = true;
        break;
      }
    }
  }
}

}  // namespace

std::vector<DepthRegionProposal> propose_depth_regions(
    const std::uint16_t* depth_data,
    int width,
    int height,
    const DepthRegionProposalOptions& options) {
  if (depth_data == nullptr || width <= 0 || height <= 0) return {};
  if (options.layer_count < 2) {
    throw std::invalid_argument("depth region proposal requires at least two layers");
  }
  if (options.minimum_area_ratio <= 0.0 ||
      options.maximum_area_ratio <= options.minimum_area_ratio ||
      options.maximum_area_ratio >= 1.0) {
    throw std::invalid_argument("invalid depth region area policy");
  }

  cv::Mat depth(height, width, CV_16UC1, const_cast<std::uint16_t*>(depth_data));
  double minimum_depth = 0.0;
  double maximum_depth = 0.0;
  cv::minMaxLoc(depth, &minimum_depth, &maximum_depth);
  if (maximum_depth <= minimum_depth) return {};

  const double frame_area = static_cast<double>(width) * height;
  const double minimum_area = options.minimum_area_ratio * frame_area;
  const double maximum_area = options.maximum_area_ratio * frame_area;
  const double layer_width =
      (maximum_depth - minimum_depth + 1.0) / options.layer_count;
  const cv::Mat open_kernel =
      cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3));
  const cv::Mat close_kernel =
      cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 5));

  std::vector<DepthRegionProposal> proposals;
  for (int layer = 0; layer < options.layer_count; ++layer) {
    const double lower = minimum_depth + layer * layer_width;
    const double upper = layer == options.layer_count - 1
        ? maximum_depth
        : minimum_depth + (layer + 1) * layer_width - 1.0;
    cv::Mat layer_mask;
    cv::inRange(depth, cv::Scalar(lower), cv::Scalar(upper), layer_mask);
    cv::morphologyEx(layer_mask, layer_mask, cv::MORPH_OPEN, open_kernel);
    cv::morphologyEx(layer_mask, layer_mask, cv::MORPH_CLOSE, close_kernel);

    cv::Mat labels;
    cv::Mat stats;
    cv::Mat centroids;
    const int component_count = cv::connectedComponentsWithStats(
        layer_mask, labels, stats, centroids, 8, CV_32S);
    for (int component = 1; component < component_count; ++component) {
      const int area = stats.at<int>(component, cv::CC_STAT_AREA);
      if (area < minimum_area || area > maximum_area) continue;

      DepthRegionProposal proposal;
      proposal.bbox = cv::Rect(
          stats.at<int>(component, cv::CC_STAT_LEFT),
          stats.at<int>(component, cv::CC_STAT_TOP),
          stats.at<int>(component, cv::CC_STAT_WIDTH),
          stats.at<int>(component, cv::CC_STAT_HEIGHT));
      if (proposal.bbox.area() > maximum_area) continue;
      cv::compare(labels, component, proposal.mask, cv::CMP_EQ);
      proposal.mask.convertTo(proposal.mask, CV_8UC1, 1.0 / 255.0);
      proposal.mean_depth = cv::mean(depth, proposal.mask)[0];
      proposals.push_back(std::move(proposal));
    }
  }

  merge_adjacent_proposals(proposals, options.adjacent_layer_merge_iom);
  std::sort(proposals.begin(), proposals.end(),
            [](const DepthRegionProposal& left,
               const DepthRegionProposal& right) {
              if (left.bbox.x != right.bbox.x) return left.bbox.x < right.bbox.x;
              if (left.bbox.y != right.bbox.y) return left.bbox.y < right.bbox.y;
              if (left.bbox.area() != right.bbox.area()) {
                return left.bbox.area() > right.bbox.area();
              }
              return left.mean_depth < right.mean_depth;
            });
  return proposals;
}

}  // namespace svp::vision::visual_entity_internal
