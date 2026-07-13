#include "svp/vision/visual_entity_detector.hpp"

#include "svp/models/manifest.hpp"
#include "svp/models/verification.hpp"

#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>
#include <optional>
#include <stdexcept>

namespace svp::vision {
namespace {

constexpr int kDetectorInputSize = 384;
constexpr std::size_t kDetectorQueryCount = 300;
constexpr std::size_t kDetectorClassCount = 91;

struct DetectionCandidate {
  VisualEntityDetection detection;
  std::size_t class_index = 0;
};

std::optional<std::filesystem::path> find_model_bundle_dir(
    const std::filesystem::path& cache_root,
    const std::string& model_id) {
  const auto direct = cache_root / model_id;
  if (std::filesystem::exists(direct / "model.svpmodel.json")) return direct;
  if (!std::filesystem::exists(cache_root)) return std::nullopt;
  for (const auto& entry : std::filesystem::directory_iterator(cache_root)) {
    if (!entry.is_directory()) continue;
    const auto manifest_path = entry.path() / "model.svpmodel.json";
    if (!std::filesystem::exists(manifest_path)) continue;
    try {
      if (svp::models::load_model_bundle_manifest(manifest_path).model_id ==
          model_id) {
        return entry.path();
      }
    } catch (...) {
    }
  }
  return std::nullopt;
}

double iou(const VisualEntityDetection& left,
           const VisualEntityDetection& right) {
  const int x0 = std::max(left.box_px[0], right.box_px[0]);
  const int y0 = std::max(left.box_px[1], right.box_px[1]);
  const int x1 = std::min(left.box_px[2], right.box_px[2]);
  const int y1 = std::min(left.box_px[3], right.box_px[3]);
  const double intersection =
      static_cast<double>(std::max(0, x1 - x0)) * std::max(0, y1 - y0);
  const double left_area =
      static_cast<double>(left.box_px[2] - left.box_px[0]) *
      (left.box_px[3] - left.box_px[1]);
  const double right_area =
      static_cast<double>(right.box_px[2] - right.box_px[0]) *
      (right.box_px[3] - right.box_px[1]);
  const double union_area = left_area + right_area - intersection;
  return union_area > 0.0 ? intersection / union_area : 0.0;
}

double intersection_over_minimum_area(
    const VisualEntityDetection& left,
    const VisualEntityDetection& right) {
  const int x0 = std::max(left.box_px[0], right.box_px[0]);
  const int y0 = std::max(left.box_px[1], right.box_px[1]);
  const int x1 = std::min(left.box_px[2], right.box_px[2]);
  const int y1 = std::min(left.box_px[3], right.box_px[3]);
  const double intersection =
      static_cast<double>(std::max(0, x1 - x0)) * std::max(0, y1 - y0);
  const double left_area =
      static_cast<double>(left.box_px[2] - left.box_px[0]) *
      (left.box_px[3] - left.box_px[1]);
  const double right_area =
      static_cast<double>(right.box_px[2] - right.box_px[0]) *
      (right.box_px[3] - right.box_px[1]);
  const double minimum_area = std::min(left_area, right_area);
  return minimum_area > 0.0 ? intersection / minimum_area : 0.0;
}

std::vector<float> prepare_input(const ColorRasterFrame& frame) {
  cv::Mat source(frame.height, frame.width, CV_8UC3);
  for (int y = 0; y < frame.height; ++y) {
    for (int x = 0; x < frame.width; ++x) {
      const auto& pixel =
          frame.pixels[static_cast<std::size_t>(y) * frame.width + x];
      source.at<cv::Vec3b>(y, x) = {pixel.b, pixel.g, pixel.r};
    }
  }
  cv::Mat resized;
  cv::resize(source, resized,
             cv::Size(kDetectorInputSize, kDetectorInputSize));
  std::vector<float> input(
      static_cast<std::size_t>(kDetectorInputSize) * kDetectorInputSize * 3);
  const std::size_t plane =
      static_cast<std::size_t>(kDetectorInputSize) * kDetectorInputSize;
  for (int y = 0; y < kDetectorInputSize; ++y) {
    for (int x = 0; x < kDetectorInputSize; ++x) {
      const auto pixel = resized.at<cv::Vec3b>(y, x);
      const std::size_t index =
          static_cast<std::size_t>(y) * kDetectorInputSize + x;
      input[index] = static_cast<float>(pixel[2]) / 255.0F;
      input[plane + index] = static_cast<float>(pixel[1]) / 255.0F;
      input[2 * plane + index] = static_cast<float>(pixel[0]) / 255.0F;
    }
  }
  return input;
}

}  // namespace

VisualEntityDetectorRuntime load_visual_entity_detector(
    const std::filesystem::path& model_cache_root,
    const VisualEntityDetectorOptions& options) {
  VisualEntityDetectorRuntime runtime;
  runtime.options = options;
  if (options.confidence_threshold <= 0.0 ||
      options.confidence_threshold > 1.0 ||
      options.nms_iou_threshold <= 0.0 ||
      options.nms_iou_threshold > 1.0 ||
      options.nms_containment_threshold <= 0.0 ||
      options.nms_containment_threshold > 1.0 ||
      options.cross_category_duplicate_iou_threshold <= 0.0 ||
      options.cross_category_duplicate_iou_threshold > 1.0 ||
      options.category_evidence_confidence_threshold <
          options.confidence_threshold ||
      options.category_evidence_confidence_threshold > 1.0) {
    runtime.blocker = "invalid visual entity detector thresholds";
    return runtime;
  }
  const auto model_dir = find_model_bundle_dir(model_cache_root, options.model_id);
  if (!model_dir) {
    runtime.blocker = "visual entity detector bundle not found";
    return runtime;
  }
  try {
    const auto manifest = svp::models::load_model_bundle_manifest(
        *model_dir / "model.svpmodel.json");
    const auto verification =
        svp::models::verify_manifest_files(manifest, *model_dir);
    if (!verification.ok()) {
      runtime.blocker = "visual entity detector bundle verification failed";
      return runtime;
    }
    svp::models::OnnxSessionOptions session_options;
    session_options.execution_provider = options.execution_provider;
    auto session = svp::models::OnnxSession::load(
        manifest, *model_dir, session_options);
    runtime.session =
        std::make_unique<svp::models::OnnxSession>(std::move(session));
    runtime.model_refs.push_back(options.model_id);
  } catch (const std::exception& error) {
    runtime.blocker = error.what();
  }
  return runtime;
}

std::vector<VisualEntityDetection> detect_visual_entities(
    VisualEntityDetectorRuntime& runtime,
    const ColorRasterFrame& frame) {
  if (!runtime.session || frame.width <= 0 || frame.height <= 0 ||
      frame.pixels.size() !=
          static_cast<std::size_t>(frame.width) * frame.height) {
    return {};
  }
  const auto input = prepare_input(frame);
  const auto io = runtime.session->io_spec();
  if (io.inputs.empty()) return {};
  const auto output = runtime.session->run_raw_with_shape(
      io.inputs.front().name,
      input.data(), input.size(),
      {1, 3, kDetectorInputSize, kDetectorInputSize});
  const std::size_t box_values = kDetectorQueryCount * 4;
  const std::size_t expected_values =
      box_values + kDetectorQueryCount * kDetectorClassCount;
  if (output.first.size() != expected_values) {
    throw std::runtime_error("unexpected RF-DETR detector output shape");
  }

  std::vector<DetectionCandidate> candidates;
  for (std::size_t index = 0; index < kDetectorQueryCount; ++index) {
    double confidence = 0.0;
    std::size_t strongest_class_index = 0;
    const std::size_t logits_offset = box_values + index * kDetectorClassCount;
    for (std::size_t class_index = 0; class_index < kDetectorClassCount;
         ++class_index) {
      const double logit = output.first[logits_offset + class_index];
      const double class_confidence = 1.0 / (1.0 + std::exp(-logit));
      if (class_confidence > confidence) {
        confidence = class_confidence;
        strongest_class_index = class_index;
      }
    }
    if (confidence < runtime.options.confidence_threshold) continue;
    const std::size_t box_offset = index * 4;
    const double center_x = output.first[box_offset];
    const double center_y = output.first[box_offset + 1];
    const double width = output.first[box_offset + 2];
    const double height = output.first[box_offset + 3];
    VisualEntityDetection detection;
    detection.box_px[0] = static_cast<int>(std::floor(
        (center_x - width / 2.0) * frame.width));
    detection.box_px[1] = static_cast<int>(std::floor(
        (center_y - height / 2.0) * frame.height));
    detection.box_px[2] = static_cast<int>(std::ceil(
        (center_x + width / 2.0) * frame.width));
    detection.box_px[3] = static_cast<int>(std::ceil(
        (center_y + height / 2.0) * frame.height));
    detection.box_px[0] = std::clamp(detection.box_px[0], 0, frame.width);
    detection.box_px[1] = std::clamp(detection.box_px[1], 0, frame.height);
    detection.box_px[2] = std::clamp(detection.box_px[2], 0, frame.width);
    detection.box_px[3] = std::clamp(detection.box_px[3], 0, frame.height);
    const double area_ratio =
        static_cast<double>(detection.box_px[2] - detection.box_px[0]) *
        (detection.box_px[3] - detection.box_px[1]) /
        (static_cast<double>(frame.width) * frame.height);
    if (area_ratio < runtime.options.minimum_area_ratio ||
        area_ratio > runtime.options.maximum_area_ratio) {
      continue;
    }
    detection.confidence = confidence;
    if (confidence >= runtime.options.category_evidence_confidence_threshold) {
      detection.category_index = static_cast<int>(strongest_class_index);
    }
    candidates.push_back({detection, strongest_class_index});
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const DetectionCandidate& left,
               const DetectionCandidate& right) {
              if (left.detection.confidence != right.detection.confidence) {
                return left.detection.confidence > right.detection.confidence;
              }
              return std::lexicographical_compare(
                  std::begin(left.detection.box_px),
                  std::end(left.detection.box_px),
                  std::begin(right.detection.box_px),
                  std::end(right.detection.box_px));
            });
  std::vector<DetectionCandidate> retained;
  for (const auto& candidate : candidates) {
    const bool suppressed = std::any_of(
        retained.begin(), retained.end(), [&](const auto& existing) {
          const double overlap_iou =
              iou(candidate.detection, existing.detection);
          if (candidate.class_index != existing.class_index) {
            return overlap_iou >=
                runtime.options.cross_category_duplicate_iou_threshold;
          }
          return overlap_iou >= runtime.options.nms_iou_threshold ||
              intersection_over_minimum_area(candidate.detection,
                                             existing.detection) >=
                  runtime.options.nms_containment_threshold;
        });
    if (suppressed) continue;
    retained.push_back(candidate);
    if (retained.size() == runtime.options.maximum_detections) break;
  }
  std::vector<VisualEntityDetection> detections;
  detections.reserve(retained.size());
  for (const auto& candidate : retained) {
    detections.push_back(candidate.detection);
  }
  return detections;
}

}  // namespace svp::vision
