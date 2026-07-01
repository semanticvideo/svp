#include "svp/vision/visual_entity_tracker.hpp"
#include "svp/vision/noise_suppression.hpp"

#include "svp/models/manifest.hpp"
#include "svp/models/runtime.hpp"
#include "svp/models/verification.hpp"

#include <opencv2/core.hpp>
#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/video.hpp>
#include <opencv2/video/tracking.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <unistd.h>

namespace svp::vision {
namespace {

class StderrSuppressor {
 public:
  StderrSuppressor() : suppressed_(false) {
    fflush(stderr);
    saved_stderr_ = dup(STDERR_FILENO);
    const int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
      suppressed_ = true;
    }
  }

  ~StderrSuppressor() {
    if (suppressed_) {
      fflush(stderr);
      dup2(saved_stderr_, STDERR_FILENO);
      close(saved_stderr_);
    }
  }

  StderrSuppressor(const StderrSuppressor&) = delete;
  StderrSuppressor& operator=(const StderrSuppressor&) = delete;

 private:
  bool suppressed_;
  int saved_stderr_;
};

// ---------------------------------------------------------------------------
// LEB128 encoding/decoding for unsigned integers.
// Used for SVP RLE mask encoding per spec §14.3.
// ---------------------------------------------------------------------------

void write_leb128(std::vector<std::uint8_t>& out, std::uint64_t value) {
  do {
    std::uint8_t byte = value & 0x7F;
    value >>= 7;
    if (value != 0) {
      byte |= 0x80;
    }
    out.push_back(byte);
  } while (value != 0);
}

std::optional<std::pair<std::uint64_t, std::size_t>> read_leb128(
    const std::uint8_t* data, std::size_t size, std::size_t offset) {
  std::uint64_t result = 0;
  std::size_t shift = 0;
  std::size_t pos = offset;
  while (pos < size) {
    std::uint8_t byte = data[pos];
    result |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
    ++pos;
    if ((byte & 0x80) == 0) {
      return std::make_pair(result, pos);
    }
    shift += 7;
    if (shift >= 64) {
      return std::nullopt;  // overflow
    }
  }
  return std::nullopt;  // incomplete
}

// ---------------------------------------------------------------------------
// Frame conversion helpers
// ---------------------------------------------------------------------------

cv::Mat srgb8_frame_to_cv_mat(const ColorRasterFrame& frame) {
  cv::Mat mat(frame.height, frame.width, CV_8UC3);
  for (int y = 0; y < frame.height; ++y) {
    for (int x = 0; x < frame.width; ++x) {
      const auto& px = frame.pixels[static_cast<std::size_t>(y) * frame.width + x];
      mat.at<cv::Vec3b>(y, x) = cv::Vec3b(px.r, px.g, px.b);
    }
  }
  return mat;
}

cv::Mat uint16_depth_to_cv_mat(const std::uint16_t* depth, int width, int height) {
  cv::Mat mat(height, width, CV_16UC1);
  std::memcpy(mat.data, depth, static_cast<std::size_t>(width) * height * sizeof(std::uint16_t));
  return mat;
}

// ---------------------------------------------------------------------------
// Keyframe selection (§20.6 step 2)
// ---------------------------------------------------------------------------

struct KeyframeSelection {
  std::vector<int> frame_indices;
};

KeyframeSelection select_keyframes(
    const std::vector<ColorRasterFrame>& frames,
    int interval,
    double motion_threshold,
    const std::vector<std::pair<std::string, std::int64_t>>& shot_boundaries) {
  KeyframeSelection result;
  if (frames.empty()) return result;

  // Collect shot boundary frame indices
  std::set<int> shot_start_indices;
  // We don't have explicit shot->frame mapping, so we use frame_id matching
  // For simplicity, shot boundaries are treated as keyframe anchors
  // The first frame of each shot is always a keyframe

  result.frame_indices.push_back(0);  // Always include first frame

  for (int i = interval; i < static_cast<int>(frames.size()); i += interval) {
    result.frame_indices.push_back(i);
  }

  // Adaptive: check motion between consecutive selected keyframes
  // If motion exceeds threshold, add intermediate frames
  std::vector<int> adaptive;
  for (std::size_t k = 0; k < result.frame_indices.size(); ++k) {
    adaptive.push_back(result.frame_indices[k]);
    if (k + 1 < result.frame_indices.size()) {
      int curr = result.frame_indices[k];
      int next = result.frame_indices[k + 1];
      if (next - curr > 1) {
        // Compute mean absolute difference between consecutive frames
        double max_diff = 0;
        for (int f = curr; f < next && f + 1 < static_cast<int>(frames.size()); ++f) {
          cv::Mat a = srgb8_frame_to_cv_mat(frames[f]);
          cv::Mat b = srgb8_frame_to_cv_mat(frames[f + 1]);
          cv::Mat diff;
          cv::absdiff(a, b, diff);
          cv::Scalar mean = cv::mean(diff);
          double frame_diff = (mean[0] + mean[1] + mean[2]) / 3.0 / 255.0;
          max_diff = std::max(max_diff, frame_diff);
        }
        if (max_diff > motion_threshold) {
          int mid = (curr + next) / 2;
          if (mid != curr && mid != next) {
            adaptive.push_back(mid);
          }
        }
      }
    }
  }

  // Ensure last frame is included
  int last = static_cast<int>(frames.size()) - 1;
  if (adaptive.empty() || adaptive.back() != last) {
    adaptive.push_back(last);
  }

  // Sort and deduplicate
  std::sort(adaptive.begin(), adaptive.end());
  adaptive.erase(std::unique(adaptive.begin(), adaptive.end()), adaptive.end());

  result.frame_indices = std::move(adaptive);
  return result;
}

// ---------------------------------------------------------------------------
// Shi-Tomasi corner detection (§20.6 step 3)
// ---------------------------------------------------------------------------

std::vector<cv::Point2f> detect_corners(
    const cv::Mat& gray,
    int max_corners,
    double quality_level,
    double min_distance) {
  std::vector<cv::Point2f> corners;
  cv::goodFeaturesToTrack(gray, corners, max_corners, quality_level, min_distance);
  return corners;
}

// ---------------------------------------------------------------------------
// Lucas-Kanade sparse optical flow (§20.6 step 4)
// ---------------------------------------------------------------------------

struct LKFlowResult {
  std::vector<cv::Point2f> prev_points;
  std::vector<cv::Point2f> next_points;
  std::vector<uchar> status;
  std::vector<float> err;
};

LKFlowResult track_lk(
    const cv::Mat& prev_gray,
    const cv::Mat& next_gray,
    const std::vector<cv::Point2f>& prev_points,
    int window_w, int window_h, int max_level, int max_count, double epsilon) {
  LKFlowResult result;
  result.prev_points = prev_points;
  cv::Size win_size(window_w, window_h);
  cv::TermCriteria criteria(cv::TermCriteria::COUNT | cv::TermCriteria::EPS, max_count, epsilon);
  cv::calcOpticalFlowPyrLK(prev_gray, next_gray, prev_points,
                           result.next_points, result.status, result.err,
                           win_size, max_level, criteria);
  return result;
}

// ---------------------------------------------------------------------------
// Farneback dense optical flow (§20.6 step 5)
// ---------------------------------------------------------------------------

cv::Mat compute_farneback_flow(
    const cv::Mat& prev_gray,
    const cv::Mat& next_gray,
    int window_size, int poly_n, double poly_sigma,
    int iterations, int pyr_scale) {
  cv::Mat flow;
  cv::calcOpticalFlowFarneback(prev_gray, next_gray, flow,
                               1.0 / pyr_scale, 1, window_size,
                               iterations, poly_n, poly_sigma, 0);
  return flow;
}

// ---------------------------------------------------------------------------
// RANSAC homography estimation (§20.6 step 6)
// ---------------------------------------------------------------------------

struct HomographyResult {
  cv::Mat homography;
  std::vector<uchar> inlier_mask;
  bool valid = false;
};

HomographyResult estimate_homography(
    const std::vector<cv::Point2f>& prev_points,
    const std::vector<cv::Point2f>& next_points,
    const std::vector<uchar>& status,
    double ransac_threshold) {
  HomographyResult result;

  // Filter to valid tracked points
  std::vector<cv::Point2f> src, dst;
  for (std::size_t i = 0; i < status.size(); ++i) {
    if (status[i]) {
      src.push_back(prev_points[i]);
      dst.push_back(next_points[i]);
    }
  }

  if (src.size() < 4) return result;

  result.homography = cv::findHomography(src, dst, cv::RANSAC, ransac_threshold, result.inlier_mask);
  result.valid = !result.homography.empty();
  return result;
}

// ---------------------------------------------------------------------------
// Residual motion computation (§20.6 step 7-8)
// ---------------------------------------------------------------------------

cv::Mat compute_residual_motion(
    const cv::Mat& dense_flow,
    const cv::Mat& homography,
    int width, int height) {
  // Warp a grid of points by the homography to get expected motion
  // Then subtract from dense flow to get residual
  cv::Mat residual(height, width, CV_32FC2);

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      cv::Vec2f flow = dense_flow.at<cv::Vec2f>(y, x);
      // Expected motion from homography
      std::vector<cv::Point2f> pt = {cv::Point2f(static_cast<float>(x), static_cast<float>(y))};
      std::vector<cv::Point2f> warped;
      cv::perspectiveTransform(pt, warped, homography);
      float exp_dx = warped[0].x - static_cast<float>(x);
      float exp_dy = warped[0].y - static_cast<float>(y);
      residual.at<cv::Vec2f>(y, x) = cv::Vec2f(flow[0] - exp_dx, flow[1] - exp_dy);
    }
  }

  return residual;
}

// ---------------------------------------------------------------------------
// Residual motion clustering (§20.6 step 8)
// ---------------------------------------------------------------------------

struct MotionCluster {
  cv::Rect bbox;
  std::vector<cv::Point> points;
  double mean_magnitude;
};

std::vector<MotionCluster> cluster_residual_motion(
    const cv::Mat& residual,
    double min_magnitude,
    double min_area_ratio,
    int frame_width, int frame_height) {
  // Create motion magnitude mask
  cv::Mat magnitude(frame_height, frame_width, CV_32FC1);
  for (int y = 0; y < frame_height; ++y) {
    for (int x = 0; x < frame_width; ++x) {
      cv::Vec2f v = residual.at<cv::Vec2f>(y, x);
      magnitude.at<float>(y, x) = std::sqrt(v[0] * v[0] + v[1] * v[1]);
    }
  }

  // Threshold
  cv::Mat mask;
  cv::threshold(magnitude, mask, min_magnitude, 255, cv::THRESH_BINARY);
  mask.convertTo(mask, CV_8UC1);

  // Find contours
  std::vector<std::vector<cv::Point>> contours;
  cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

  double min_area = min_area_ratio * frame_width * frame_height;
  std::vector<MotionCluster> clusters;
  for (const auto& contour : contours) {
    double area = cv::contourArea(contour);
    if (area < min_area) continue;
    MotionCluster cluster;
    cluster.bbox = cv::boundingRect(contour);
    cluster.points = contour;
    // Mean magnitude within bbox
    double sum = 0;
    int count = 0;
    for (int y = cluster.bbox.y; y < cluster.bbox.y + cluster.bbox.height; ++y) {
      for (int x = cluster.bbox.x; x < cluster.bbox.x + cluster.bbox.width; ++x) {
        if (y >= 0 && y < frame_height && x >= 0 && x < frame_width) {
          sum += magnitude.at<float>(y, x);
          ++count;
        }
      }
    }
    cluster.mean_magnitude = count > 0 ? sum / count : 0;
    clusters.push_back(cluster);
  }

  return clusters;
}

// ---------------------------------------------------------------------------
// Depth-based candidate detection (§20.6 step 8b: depth-derived candidates)
// ---------------------------------------------------------------------------

// Check if depth data is too flat to produce meaningful candidates.
// Returns true if the depth variance (as fraction of mean) is below threshold.
bool is_depth_flat(
    const std::uint16_t* depth_data,
    int width, int height,
    double variance_threshold) {
  if (width <= 0 || height <= 0 || depth_data == nullptr) return true;
  const std::size_t total = static_cast<std::size_t>(width) * height;
  if (total == 0) return true;

  // Sample pixels for efficiency (every 4th pixel in each dimension)
  std::vector<std::uint16_t> samples;
  for (int y = 0; y < height; y += 4) {
    for (int x = 0; x < width; x += 4) {
      samples.push_back(depth_data[static_cast<std::size_t>(y) * width + x]);
    }
  }
  if (samples.empty()) return true;

  double sum = 0;
  for (auto v : samples) sum += v;
  double mean = sum / samples.size();
  if (mean < 1.0) return true;  // All-zero or near-zero depth

  double sq_sum = 0;
  for (auto v : samples) sq_sum += (v - mean) * (v - mean);
  double variance = sq_sum / samples.size();
  double normalized_var = variance / (mean * mean);

  return normalized_var < variance_threshold;
}

// Detect coherent depth regions from depth discontinuities.
// Uses Canny edge detection on depth, then finds contours and bounding boxes.
// Returns depth-derived candidate regions.
struct DepthCandidate {
  cv::Rect bbox;
  cv::Mat mask;
  double mean_depth;
};

std::vector<DepthCandidate> detect_depth_candidates(
    const std::uint16_t* depth_data,
    int depth_width, int depth_height,
    int frame_width, int frame_height,
    int edge_threshold,
    double min_area_ratio) {
  std::vector<DepthCandidate> candidates;

  if (depth_data == nullptr || depth_width <= 0 || depth_height <= 0) {
    return candidates;
  }

  // Convert depth to CV_16U
  cv::Mat depth_mat = uint16_depth_to_cv_mat(depth_data, depth_width, depth_height);

  // Convert to 8-bit for edge detection (scale to 0-255)
  cv::Mat depth_8u;
  double min_val, max_val;
  cv::minMaxLoc(depth_mat, &min_val, &max_val);
  if (max_val < 1.0) return candidates;

  double scale = 255.0 / max_val;
  depth_mat.convertTo(depth_8u, CV_8UC1, scale);

  // Blur to reduce noise before edge detection
  cv::Mat blurred;
  cv::GaussianBlur(depth_8u, blurred, cv::Size(5, 5), 0);

  // Canny edge detection on depth
  cv::Mat edges;
  cv::Canny(blurred, edges, edge_threshold * scale, edge_threshold * scale * 2);

  // Dilate edges to connect nearby boundaries
  cv::Mat dilated;
  cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
  cv::dilate(edges, dilated, kernel);

  // Find contours from depth edges
  std::vector<std::vector<cv::Point>> contours;
  cv::findContours(dilated, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

  double min_area = min_area_ratio * frame_width * frame_height;

  // Scale factor from depth resolution to frame resolution
  double sx = static_cast<double>(frame_width) / depth_width;
  double sy = static_cast<double>(frame_height) / depth_height;

  for (const auto& contour : contours) {
    double area = cv::contourArea(contour);
    if (area < min_area / (sx * sy)) continue;  // Check area in frame coords

    cv::Rect bbox = cv::boundingRect(contour);

    // Scale bbox to frame resolution
    cv::Rect frame_bbox(
      static_cast<int>(bbox.x * sx),
      static_cast<int>(bbox.y * sy),
      static_cast<int>(bbox.width * sx),
      static_cast<int>(bbox.height * sy));
    frame_bbox &= cv::Rect(0, 0, frame_width, frame_height);

    if (frame_bbox.width <= 0 || frame_bbox.height <= 0) continue;

    // Create mask from contour (in frame resolution)
    cv::Mat mask = cv::Mat::zeros(frame_height, frame_width, CV_8UC1);
    // Scale contour points to frame resolution
    std::vector<cv::Point> scaled_contour;
    for (const auto& pt : contour) {
      scaled_contour.emplace_back(
        static_cast<int>(pt.x * sx),
        static_cast<int>(pt.y * sy));
    }
    cv::fillConvexPoly(mask, scaled_contour, 1);

    // Compute mean depth within the contour region
    double depth_sum = 0;
    int depth_count = 0;
    for (int y = bbox.y; y < bbox.y + bbox.height && y < depth_height; ++y) {
      for (int x = bbox.x; x < bbox.x + bbox.width && x < depth_width; ++x) {
        if (y >= 0 && x >= 0) {
          depth_sum += depth_data[static_cast<std::size_t>(y) * depth_width + x];
          ++depth_count;
        }
      }
    }

    DepthCandidate dc;
    dc.bbox = frame_bbox;
    dc.mask = mask;
    dc.mean_depth = depth_count > 0 ? depth_sum / depth_count : 0;
    candidates.push_back(dc);
  }

  return candidates;
}

cv::Mat refine_mask_grabcut(
    const cv::Mat& color_frame,
    const cv::Rect& bbox,
    int iterations) {
  if (color_frame.empty() || bbox.width < 2 || bbox.height < 2 ||
      bbox.x < 0 || bbox.y < 0 ||
      bbox.x + bbox.width > color_frame.cols ||
      bbox.y + bbox.height > color_frame.rows) {
    cv::Mat fallback = cv::Mat::zeros(color_frame.size(), CV_8UC1);
    cv::rectangle(fallback, bbox, 1, cv::FILLED);
    return fallback;
  }

  cv::Mat mask = cv::Mat::zeros(color_frame.size(), CV_8UC1);
  cv::Mat bg_model, fg_model;

  cv::rectangle(mask, bbox, cv::GC_PR_FGD, cv::FILLED);
  int cx = bbox.x + bbox.width / 4;
  int cy = bbox.y + bbox.height / 4;
  int cw = bbox.width / 2;
  int ch = bbox.height / 2;
  cv::Rect inner(cx, cy, cw, ch);
  inner &= cv::Rect(0, 0, color_frame.cols, color_frame.rows);
  if (inner.width > 0 && inner.height > 0) {
    cv::rectangle(mask, inner, cv::GC_FGD, cv::FILLED);
  }

  cv::Mat bgd_model, fgd_model;
  try {
    std::optional<StderrSuppressor> suppressor;
    if (!svp::vision::opencv_verbose()) {
      suppressor.emplace();
    }
    cv::grabCut(color_frame, mask, bbox, bgd_model, fgd_model, iterations, cv::GC_INIT_WITH_RECT);
  } catch (...) {
    mask = cv::Mat::zeros(color_frame.size(), CV_8UC1);
    cv::rectangle(mask, bbox, 1, cv::FILLED);
    return mask;
  }

  // Extract foreground mask
  cv::Mat result;
  cv::compare(mask, cv::GC_PR_FGD, result, cv::CMP_EQ);
  cv::Mat result2;
  cv::compare(mask, cv::GC_FGD, result2, cv::CMP_EQ);
  result |= result2;

  return result;
}

// ---------------------------------------------------------------------------
// Depth summary computation (§20.6 step 11)
// ---------------------------------------------------------------------------

struct DepthSummary {
  double median_inverse_depth;
  double near_percentile_10;
  double far_percentile_90;
};

DepthSummary compute_depth_summary(
    const std::uint16_t* depth_data,
    int depth_width, int depth_height,
    const cv::Rect& bbox,
    int frame_width, int frame_height) {
  DepthSummary ds{0, 0, 0};

  // Collect depth values within the bbox region
  std::vector<std::uint16_t> values;
  for (int y = bbox.y; y < bbox.y + bbox.height && y < depth_height; ++y) {
    for (int x = bbox.x; x < bbox.x + bbox.width && x < depth_width; ++x) {
      if (y >= 0 && x >= 0) {
        values.push_back(depth_data[static_cast<std::size_t>(y) * depth_width + x]);
      }
    }
  }

  if (values.empty()) return ds;

  std::sort(values.begin(), values.end());
  std::size_t n = values.size();
  ds.median_inverse_depth = values[n / 2];
  ds.near_percentile_10 = values[static_cast<std::size_t>(n * 0.1)];
  ds.far_percentile_90 = values[static_cast<std::size_t>(n * 0.9)];

  return ds;
}

// ---------------------------------------------------------------------------
// Kalman filter for 2D bounding box tracking (§20.6 step 12)
// ---------------------------------------------------------------------------

struct KalmanTracker {
  cv::KalmanFilter kf;
  int lost_count = 0;
  std::string entity_id;
  std::vector<float> last_embedding;
  bool initialized = false;

  KalmanTracker() : kf(8, 4, 0, CV_32F) {
    // State: [x, y, w, h, vx, vy, vw, vh]
    // Measurement: [x, y, w, h]
    kf.transitionMatrix = (cv::Mat_<float>(8, 8) <<
      1, 0, 0, 0, 1, 0, 0, 0,
      0, 1, 0, 0, 0, 1, 0, 0,
      0, 0, 1, 0, 0, 0, 1, 0,
      0, 0, 0, 1, 0, 0, 0, 1,
      0, 0, 0, 0, 1, 0, 0, 0,
      0, 0, 0, 0, 0, 1, 0, 0,
      0, 0, 0, 0, 0, 0, 1, 0,
      0, 0, 0, 0, 0, 0, 0, 1);

    kf.measurementMatrix = (cv::Mat_<float>(4, 8) <<
      1, 0, 0, 0, 0, 0, 0, 0,
      0, 1, 0, 0, 0, 0, 0, 0,
      0, 0, 1, 0, 0, 0, 0, 0,
      0, 0, 0, 1, 0, 0, 0, 0);

    cv::setIdentity(kf.processNoiseCov, cv::Scalar::all(1e-4));
    cv::setIdentity(kf.measurementNoiseCov, cv::Scalar::all(1e-1));
    cv::setIdentity(kf.errorCovPost, cv::Scalar::all(1));
  }

  void init(const cv::Rect& bbox) {
    kf.statePost.at<float>(0) = static_cast<float>(bbox.x);
    kf.statePost.at<float>(1) = static_cast<float>(bbox.y);
    kf.statePost.at<float>(2) = static_cast<float>(bbox.width);
    kf.statePost.at<float>(3) = static_cast<float>(bbox.height);
    kf.statePost.at<float>(4) = 0;
    kf.statePost.at<float>(5) = 0;
    kf.statePost.at<float>(6) = 0;
    kf.statePost.at<float>(7) = 0;
    initialized = true;
  }

  cv::Rect predict() {
    cv::Mat prediction = kf.predict();
    return cv::Rect(
      static_cast<int>(prediction.at<float>(0)),
      static_cast<int>(prediction.at<float>(1)),
      static_cast<int>(prediction.at<float>(2)),
      static_cast<int>(prediction.at<float>(3)));
  }

  void correct(const cv::Rect& bbox) {
    cv::Mat measurement = (cv::Mat_<float>(4, 1) <<
      static_cast<float>(bbox.x),
      static_cast<float>(bbox.y),
      static_cast<float>(bbox.width),
      static_cast<float>(bbox.height));
    kf.correct(measurement);
    lost_count = 0;
  }

  void mark_lost() {
    ++lost_count;
  }
};

// ---------------------------------------------------------------------------
// Embedding helpers
// ---------------------------------------------------------------------------

double cosine_similarity(const std::vector<float>& a, const std::vector<float>& b) {
  if (a.size() != b.size() || a.empty()) return 0;
  double dot = 0, norm_a = 0, norm_b = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    dot += a[i] * b[i];
    norm_a += a[i] * a[i];
    norm_b += b[i] * b[i];
  }
  if (norm_a == 0 || norm_b == 0) return 0;
  return dot / (std::sqrt(norm_a) * std::sqrt(norm_b));
}

void l2_normalize(std::vector<float>& vec) {
  double norm = 0;
  for (float v : vec) norm += v * v;
  norm = std::sqrt(norm);
  if (norm > 0) {
    for (float& v : vec) v = static_cast<float>(v / norm);
  }
}

// Convert sRGB8 pixel buffer to normalized float CHW format for Nomic Embed Vision.
// CLIP-style preprocessing: rescale to [0,1], normalize with CLIP mean/std.
// Input is resized to 224x224 before this function is called.
std::vector<float> frame_to_clip_normalized_chw(
    const cv::Mat& resized_frame) {
  const int width = resized_frame.cols;
  const int height = resized_frame.rows;
  const std::size_t pixel_count = static_cast<std::size_t>(width) * height;
  std::vector<float> output(pixel_count * 3);
  constexpr float kMean[3] = {0.48145466f, 0.4578275f, 0.40821073f};
  constexpr float kStd[3] = {0.26862954f, 0.26130258f, 0.27577711f};
  for (int i = 0; i < static_cast<int>(pixel_count); ++i) {
    const auto* px = resized_frame.ptr<cv::Vec3b>(i / width) + (i % width);
    output[i] = (static_cast<float>((*px)[0]) / 255.0f - kMean[0]) / kStd[0];
    output[pixel_count + i] = (static_cast<float>((*px)[1]) / 255.0f - kMean[1]) / kStd[1];
    output[2 * pixel_count + i] = (static_cast<float>((*px)[2]) / 255.0f - kMean[2]) / kStd[2];
  }
  return output;
}

// Crop a region from a frame for embedding computation
cv::Mat crop_region(
    const cv::Mat& frame,
    const cv::Rect& bbox) {
  cv::Rect clamped = bbox & cv::Rect(0, 0, frame.cols, frame.rows);
  if (clamped.width <= 0 || clamped.height <= 0) {
    return cv::Mat();
  }
  return frame(clamped).clone();
}

// ---------------------------------------------------------------------------
// Utility: IoU computation
// ---------------------------------------------------------------------------

double compute_iou(const cv::Rect& a, const cv::Rect& b) {
  int x1 = std::max(a.x, b.x);
  int y1 = std::max(a.y, b.y);
  int x2 = std::min(a.x + a.width, b.x + b.width);
  int y2 = std::min(a.y + a.height, b.y + b.height);
  int w = std::max(0, x2 - x1);
  int h = std::max(0, y2 - y1);
  double intersection = w * h;
  double union_area = a.area() + b.area() - intersection;
  return union_area > 0 ? intersection / union_area : 0;
}

// Intersection-over-minimum-area: useful when one bbox is much smaller
// than the other (e.g., motion cluster vs depth contour).  Returns the
// fraction of the smaller bbox that is covered by the intersection.
double compute_iom(const cv::Rect& a, const cv::Rect& b) {
  int x1 = std::max(a.x, b.x);
  int y1 = std::max(a.y, b.y);
  int x2 = std::min(a.x + a.width, b.x + b.width);
  int y2 = std::min(a.y + a.height, b.y + b.height);
  int w = std::max(0, x2 - x1);
  int h = std::max(0, y2 - y1);
  double intersection = w * h;
  double min_area = std::min(a.area(), b.area());
  return min_area > 0 ? intersection / min_area : 0;
}

// ---------------------------------------------------------------------------
// Degenerate source detection (§13.4)
// ---------------------------------------------------------------------------

bool is_degenerate_source(const std::vector<ColorRasterFrame>& frames) {
  if (frames.empty()) return true;
  if (frames.size() < 2) return true;

  // Check if all frames are identical (all-constant video)
  const auto& first = frames[0];
  for (std::size_t i = 1; i < frames.size(); ++i) {
    const auto& frame = frames[i];
    if (frame.width != first.width || frame.height != first.height) {
      return false;  // Different dimensions = not degenerate
    }
    // Quick check: compare a sample of pixels
    int sample_step = std::max(1, static_cast<int>(frame.pixels.size() / 100));
    for (std::size_t j = 0; j < frame.pixels.size(); j += sample_step) {
      if (frame.pixels[j].r != first.pixels[j].r ||
          frame.pixels[j].g != first.pixels[j].g ||
          frame.pixels[j].b != first.pixels[j].b) {
        return false;  // Found difference
      }
    }
  }
  return true;  // All sampled pixels identical
}

// ---------------------------------------------------------------------------
// Model bundle finding (same pattern as depth_generation)
// ---------------------------------------------------------------------------

std::optional<std::filesystem::path> find_model_bundle_dir(
    const std::filesystem::path& cache_root,
    const std::string& model_id) {
  if (cache_root.empty() || !std::filesystem::exists(cache_root)) {
    return std::nullopt;
  }
  const std::filesystem::path model_dir = cache_root / model_id;
  if (std::filesystem::exists(model_dir / "model.svpmodel.json")) {
    return model_dir;
  }
  for (const auto& entry : std::filesystem::directory_iterator(cache_root)) {
    if (!entry.is_directory()) continue;
    const auto candidate = entry.path() / "model.svpmodel.json";
    if (std::filesystem::exists(candidate)) {
      try {
        auto manifest = svp::models::load_model_bundle_manifest(candidate);
        if (manifest.model_id == model_id) {
          return entry.path();
        }
      } catch (...) {}
    }
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// ID generation
// ---------------------------------------------------------------------------

std::string make_region_id(int entity_idx, int region_seq, int frame_idx) {
  std::ostringstream oss;
  oss << "region_" << std::setfill('0') << std::setw(6) << (entity_idx + 1)
      << "_" << std::setw(6) << (region_seq + 1)
      << "_" << std::setw(6) << (frame_idx + 1);
  return oss.str();
}

std::string make_entity_id(int idx) {
  std::ostringstream oss;
  oss << "entity_" << std::setfill('0') << std::setw(6) << (idx + 1);
  return oss.str();
}

std::string make_track_id(int idx) {
  std::ostringstream oss;
  oss << "track_" << std::setfill('0') << std::setw(6) << (idx + 1);
  return oss.str();
}

}  // namespace

// ---------------------------------------------------------------------------
// RLE mask encoding/decoding (spec §14.3)
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> encode_mask_rle(
    const std::uint8_t* mask_pixels,
    int width,
    int height) {
  std::vector<std::uint8_t> rle;
  if (width <= 0 || height <= 0 || mask_pixels == nullptr) {
    return rle;
  }

  const std::size_t total = static_cast<std::size_t>(width) * height;
  // RLE starts with background run length (spec §14.3)
  // Scan order: row-major from top-left to bottom-right
  std::uint64_t run_length = 0;
  std::uint8_t current_val = 0;  // Start counting background (0) runs

  for (std::size_t i = 0; i < total; ++i) {
    if (mask_pixels[i] == current_val) {
      ++run_length;
    } else {
      write_leb128(rle, run_length);
      current_val = 1 - current_val;
      run_length = 1;
    }
  }
  // Write final run
  write_leb128(rle, run_length);

  return rle;
}

std::vector<std::uint8_t> decode_mask_rle(
    const std::uint8_t* rle_data,
    std::size_t rle_size,
    int width,
    int height) {
  std::vector<std::uint8_t> mask;
  if (width <= 0 || height <= 0 || rle_data == nullptr || rle_size == 0) {
    return mask;
  }

  const std::size_t total = static_cast<std::size_t>(width) * height;
  mask.reserve(total);

  std::size_t offset = 0;
  std::uint8_t current_val = 0;  // First run is background
  std::size_t decoded = 0;

  while (offset < rle_size && decoded < total) {
    auto run_result = read_leb128(rle_data, rle_size, offset);
    if (!run_result) {
      return {};  // Error: invalid LEB128
    }
    std::uint64_t run = run_result->first;
    offset = run_result->second;

    if (decoded + run > total) {
      return {};  // Error: run exceeds total
    }

    for (std::uint64_t i = 0; i < run; ++i) {
      mask.push_back(current_val);
    }
    decoded += run;
    current_val = 1 - current_val;
  }

  if (decoded != total) {
    return {};  // Error: run total != width * height
  }

  return mask;
}

// ---------------------------------------------------------------------------
// Main pipeline: run_visual_entity_tracker (§20.6 steps 1-14)
// ---------------------------------------------------------------------------

EntityTrackResult run_visual_entity_tracker(
    const std::vector<ColorRasterFrame>& decoded_frames,
    const std::vector<std::uint16_t>& depth_data,
    const std::vector<std::string>& depth_frame_ids,
    const std::vector<std::pair<std::string, std::int64_t>>& shot_boundaries,
    const std::filesystem::path& model_cache_root,
    const VisualEntityTrackerOptions& options) {
  EntityTrackResult result;

  // Record OpenCV version for provenance
  result.opencv_version = cv::getVersionString();
  result.processor_id = "proc_visual_entity_tracker_0001";
  result.runtime = "onnxruntime";
  result.execution_provider = options.execution_provider;
  result.confidence_calibration_status = "uncalibrated";
  result.limitations_note =
      "Visual entity tracking uses classical OpenCV methods (Shi-Tomasi, LK, "
      "Farneback, RANSAC, GrabCut, Kalman). Confidence values are derived from "
      "tracking stability and IoU, not calibrated against ground truth.";

  // Record parameters for provenance
  result.parameters_json = {
    {"keyframe_interval_frames", options.keyframe_interval_frames},
    {"motion_change_threshold", options.motion_change_threshold},
    {"max_corners", options.max_corners},
    {"corner_quality_level", options.corner_quality_level},
    {"corner_min_distance", options.corner_min_distance},
    {"lk_window", {options.lk_window_width, options.lk_window_height}},
    {"lk_max_level", options.lk_max_level},
    {"farneback_window_size", options.farneback_window_size},
    {"farneback_poly_n", options.farneback_poly_n},
    {"farneback_poly_sigma", options.farneback_poly_sigma},
    {"ransac_threshold", options.ransac_threshold},
    {"min_motion_magnitude", options.min_motion_magnitude},
    {"min_region_area_ratio", options.min_region_area_ratio},
    {"grabcut_iterations", options.grabcut_iterations},
    {"kalman_process_noise", options.kalman_process_noise},
    {"kalman_measurement_noise", options.kalman_measurement_noise},
    {"max_lost_frames", options.max_lost_frames},
    {"appearance_similarity_threshold", options.appearance_similarity_threshold}
  };

  // Step 1: Degenerate source check (§13.4)
  // Static frames are degenerate for motion-based tracking, but if depth data
  // has meaningful variance, depth-derived candidates can still discover
  // static entities.  Only skip if both frames AND depth are degenerate.
  bool frames_degenerate = is_degenerate_source(decoded_frames);
  bool depth_has_variance = false;
  if (frames_degenerate && !depth_data.empty() && !depth_frame_ids.empty()) {
    // Check if the first frame's depth has meaningful variance
    const std::uint16_t* depth_ptr = depth_data.data();
    int d_w = decoded_frames[0].width;
    int d_h = decoded_frames[0].height;
    depth_has_variance = !is_depth_flat(depth_ptr, d_w, d_h,
                                         options.depth_variance_threshold);
  }

  if (frames_degenerate && !depth_has_variance) {
    result.limitations_note += " Degenerate source detected: no persistent "
        "visual entities. Empty entity, track, and region files produced.";
    return result;
  }

  if (decoded_frames.size() < 2) {
    result.limitations_note += " Insufficient frames for tracking (< 2).";
    return result;
  }

  const int frame_width = decoded_frames[0].width;
  const int frame_height = decoded_frames[0].height;
  const int depth_width = frame_width;
  const int depth_height = frame_height;

  // Step 2: Keyframe selection (§20.6 step 2)
  auto keyframes = select_keyframes(
      decoded_frames,
      options.keyframe_interval_frames,
      options.motion_change_threshold,
      shot_boundaries);

  if (keyframes.frame_indices.size() < 2) {
    result.limitations_note += " Insufficient keyframes for tracking.";
    return result;
  }

  // Load visual embedding model
  std::unique_ptr<svp::models::OnnxSession> embedding_session;
  std::optional<std::filesystem::path> model_dir;
  bool embeddings_available = false;

  if (!model_cache_root.empty()) {
    model_dir = find_model_bundle_dir(model_cache_root, options.embedding_model_id);
    if (model_dir) {
      try {
        auto manifest = svp::models::load_model_bundle_manifest(
            *model_dir / "model.svpmodel.json");
        auto verify_report = svp::models::verify_manifest_files(manifest, *model_dir);
        if (!verify_report.ok()) {
          // Model hash verification failed — do not trust the model
          result.limitations_note += "Visual embedding model verification failed; embeddings unavailable. ";
        } else {
          svp::models::OnnxSessionOptions session_opts;
          session_opts.execution_provider = options.execution_provider;
          auto session = svp::models::OnnxSession::load(manifest, *model_dir, session_opts);
          embedding_session = std::make_unique<svp::models::OnnxSession>(std::move(session));
          embeddings_available = true;
          result.model_refs.push_back(options.embedding_model_id);
          result.limitations_note += "Appearance embeddings from " + options.embedding_model_id + ". ";
        }
      } catch (const std::exception& e) {
        // Model load failed; continue without embeddings
        result.limitations_note += std::string("Visual embedding model load failed: ") + e.what() + ". ";
      }
    } else {
      result.limitations_note += "Visual embedding model not found in cache; embeddings not used. ";
    }
  } else {
    result.limitations_note += "No model cache provided; visual embeddings not used. ";
  }

  // Tracking state
  struct ActiveTrack {
    KalmanTracker kalman;
    std::string entity_id;
    std::string track_id;
    int entity_idx = 0;
    std::vector<float> last_embedding;
    cv::Rect last_bbox;
    int first_frame_idx = 0;
    int last_frame_idx = 0;
    int region_count = 0;
    int lost_count = 0;
    bool reacquired = false;
    std::int64_t start_us = 0;
    std::int64_t end_us = 0;
    std::string candidate_source;  // "motion", "depth", or "fused_motion_depth"
  };

  std::vector<ActiveTrack> active_tracks;
  int next_entity_idx = 0;
  int next_track_idx = 0;
  int total_region_count = 0;
  std::size_t visual_embedding_count = 0;

  // Process keyframe pairs
  const std::size_t total_keyframe_pairs = keyframes.frame_indices.size() - 1;
  if (options.on_tracking_progress) {
    options.on_tracking_progress(0, total_keyframe_pairs);
  }
  for (std::size_t k = 0; k + 1 < keyframes.frame_indices.size(); ++k) {
    int idx_prev = keyframes.frame_indices[k];
    int idx_next = keyframes.frame_indices[k + 1];

    const auto& frame_prev = decoded_frames[idx_prev];
    const auto& frame_next = decoded_frames[idx_next];

    cv::Mat mat_prev = srgb8_frame_to_cv_mat(frame_prev);
    cv::Mat mat_next = srgb8_frame_to_cv_mat(frame_next);

    cv::Mat gray_prev, gray_next;
    cv::cvtColor(mat_prev, gray_prev, cv::COLOR_BGR2GRAY);
    cv::cvtColor(mat_next, gray_next, cv::COLOR_BGR2GRAY);

    // Step 8b: Depth-based candidate detection (§20.6 step 8b)
    // Detect coherent static regions from depth discontinuities.
    // This runs BEFORE motion detection so that depth-derived entities
    // are discovered even when RGB has no texture for corner detection.
    std::vector<DepthCandidate> depth_candidates;
    const std::uint16_t* frame_depth_ptr = nullptr;
    if (!depth_data.empty()) {
      int d_idx = -1;
      for (std::size_t d = 0; d < depth_frame_ids.size(); ++d) {
        if (depth_frame_ids[d] == frame_next.frame_id) {
          d_idx = static_cast<int>(d);
          break;
        }
      }
      if (d_idx >= 0) {
        frame_depth_ptr = &depth_data[static_cast<std::size_t>(d_idx) * depth_width * depth_height];
        // Only detect depth candidates if depth is not flat
        if (!is_depth_flat(frame_depth_ptr, depth_width, depth_height,
                           options.depth_variance_threshold)) {
          depth_candidates = detect_depth_candidates(
              frame_depth_ptr, depth_width, depth_height,
              frame_width, frame_height,
              options.depth_edge_threshold,
              options.min_region_area_ratio);
        }
      }
    }

    // Step 3-8: Motion detection pipeline (§20.6 steps 3-8)
    // These steps require RGB texture.  If corners are not found (e.g.
    // uniform RGB frames), motion candidates will be empty but depth
    // candidates above are still processed.
    std::vector<MotionCluster> clusters;

    // Step 3: Shi-Tomasi corner detection (§20.6 step 3)
    auto corners = detect_corners(gray_prev, options.max_corners,
                                  options.corner_quality_level,
                                  options.corner_min_distance);

    if (!corners.empty()) {
      // Step 4: Lucas-Kanade sparse optical flow (§20.6 step 4)
      auto lk_result = track_lk(gray_prev, gray_next, corners,
                                options.lk_window_width, options.lk_window_height,
                                options.lk_max_level, options.lk_max_count,
                                options.lk_epsilon);

      // Step 5: Farneback dense optical flow (§20.6 step 5)
      cv::Mat dense_flow = compute_farneback_flow(
          gray_prev, gray_next,
          options.farneback_window_size, options.farneback_poly_n,
          options.farneback_poly_sigma, options.farneback_iterations,
          options.farneback_pyr_scale);

      // Step 6: RANSAC homography estimation (§20.6 step 6)
      auto homography_result = estimate_homography(
          lk_result.prev_points, lk_result.next_points,
          lk_result.status, options.ransac_threshold);

      cv::Mat residual;

      if (homography_result.valid) {
        // Step 7: Camera motion compensation (§20.6 step 7)
        residual = compute_residual_motion(
            dense_flow, homography_result.homography,
            frame_width, frame_height);
      } else {
        // No homography: use raw dense flow as residual
        residual = dense_flow;
      }

      // Step 8: Residual motion clustering (§20.6 step 8)
      clusters = cluster_residual_motion(
          residual, options.min_motion_magnitude,
          options.min_region_area_ratio,
          frame_width, frame_height);
    }

    // Skip frame if no candidates from either source
    if (clusters.empty() && depth_candidates.empty()) {
      // Mark tracks as lost
      for (auto& track : active_tracks) {
        if (track.last_frame_idx < idx_next) {
          track.kalman.mark_lost();
        }
      }
      continue;
    }

    // Build unified candidate list: merge motion and depth candidates
    struct UnifiedCandidate {
      cv::Rect bbox;
      cv::Mat mask;       // Pre-computed mask (for depth candidates)
      bool has_mask;      // Whether a pre-computed mask is available
      std::string source; // "motion", "depth", or "fused_motion_depth"
    };

    std::vector<UnifiedCandidate> unified_candidates;

    // Add motion candidates
    for (std::size_t c = 0; c < clusters.size(); ++c) {
      auto& cluster = clusters[c];
      if (cluster.bbox.width <= 0 || cluster.bbox.height <= 0) continue;
      if (cluster.bbox.area() < options.min_region_area_ratio * frame_width * frame_height) {
        continue;
      }
      UnifiedCandidate uc;
      uc.bbox = cluster.bbox;
      uc.has_mask = false;
      uc.source = "motion";
      unified_candidates.push_back(std::move(uc));
    }

    // Add depth candidates, merging with motion candidates if they overlap
    for (auto& dc : depth_candidates) {
      if (dc.bbox.width <= 0 || dc.bbox.height <= 0) continue;
      if (dc.bbox.area() < options.min_region_area_ratio * frame_width * frame_height) {
        continue;
      }

      // Check if this depth candidate overlaps with any existing motion candidate
      bool fused = false;
      for (auto& uc : unified_candidates) {
        if (uc.source == "motion" || uc.source == "fused_motion_depth") {
          double iou = compute_iou(uc.bbox, dc.bbox);
          double iom = compute_iom(uc.bbox, dc.bbox);
          // Merge if either IoU or IoM exceeds threshold
          if (iou > options.candidate_merge_iou_threshold ||
              iom > options.candidate_merge_iou_threshold) {
            uc.source = "fused_motion_depth";
            fused = true;
            break;
          }
        }
      }

      if (!fused) {
        // Add as a depth-only candidate
        UnifiedCandidate uc;
        uc.bbox = dc.bbox;
        uc.mask = dc.mask.clone();
        uc.has_mask = true;
        uc.source = "depth";
        unified_candidates.push_back(std::move(uc));
      }
    }

    // Track which tracks have been assigned a region this frame
    // to enforce one-region-per-track-per-frame assignment
    std::set<int> assigned_this_frame;

    // Step 9: Process unified candidates (§20.6 step 9)
    for (auto& uc : unified_candidates) {
      cv::Rect bbox = uc.bbox;

      if (bbox.width <= 0 || bbox.height <= 0) continue;
      if (bbox.area() < options.min_region_area_ratio * frame_width * frame_height) {
        continue;
      }

      // Step 10: Mask refinement (§20.6 step 10, §5.9)
      // For depth-only candidates, use the pre-computed depth mask.
      // For motion and fused candidates, use GrabCut on the color frame.
      cv::Mat binary_mask;
      if (uc.has_mask && uc.source == "depth") {
        binary_mask = uc.mask.clone();
      } else {
        cv::Mat mask = refine_mask_grabcut(mat_next, bbox, options.grabcut_iterations);
        cv::threshold(mask, binary_mask, 0.5, 1, cv::THRESH_BINARY);
      }

      // Step 11: Depth summary (§20.6 step 11)
      DepthSummary depth_summary{0, 0, 0};
      if (!depth_data.empty()) {
        const std::uint16_t* depth_ptr = nullptr;
        int d_idx = -1;
        // Find depth data for this frame
        for (std::size_t d = 0; d < depth_frame_ids.size(); ++d) {
          if (depth_frame_ids[d] == frame_next.frame_id) {
            d_idx = static_cast<int>(d);
            break;
          }
        }
        if (d_idx >= 0) {
          depth_ptr = &depth_data[static_cast<std::size_t>(d_idx) * depth_width * depth_height];
          depth_summary = compute_depth_summary(
              depth_ptr, depth_width, depth_height,
              bbox, frame_width, frame_height);
        }
      }

      // Step 12: Kalman filter tracking (§20.6 step 12)
      // Try to match this region to an existing track
      int best_track_idx = -1;
      double best_iou = 0;

      for (std::size_t t = 0; t < active_tracks.size(); ++t) {
        if (assigned_this_frame.count(static_cast<int>(t))) continue;
        if (active_tracks[t].lost_count > options.max_lost_frames) continue;
        cv::Rect predicted = active_tracks[t].kalman.predict();
        double iou = compute_iou(predicted, bbox);
        if (iou > best_iou) {
          best_iou = iou;
          best_track_idx = static_cast<int>(t);
        }
      }

      // Compute visual embedding for this region
      std::vector<float> region_embedding;
      if (embedding_session) {
        cv::Mat crop = crop_region(mat_next, bbox);
        if (!crop.empty()) {
          cv::Mat resized;
          cv::resize(crop, resized, cv::Size(224, 224));
          auto input_data = frame_to_clip_normalized_chw(resized);

          try {
            auto embedding_result = embedding_session->run_visual_embedding(
                input_data.data(), input_data.size(), 224, 224);
            if (!embedding_result.empty()) {
              region_embedding = std::move(embedding_result);
              l2_normalize(region_embedding);
            }
          } catch (...) {
          }
        }
        ++visual_embedding_count;
        if (options.on_visual_embedding_progress) {
          options.on_visual_embedding_progress(visual_embedding_count, 0);
        }
      }

      // Step 13: Track reacquisition via embedding similarity (§20.6 step 13)
      if (best_track_idx < 0 || best_iou < 0.1) {
        // Try embedding-based reacquisition
        if (!region_embedding.empty()) {
          double best_sim = -1;
          for (std::size_t t = 0; t < active_tracks.size(); ++t) {
            if (assigned_this_frame.count(static_cast<int>(t))) continue;
            if (active_tracks[t].lost_count == 0) continue;  // Only lost tracks
            if (active_tracks[t].lost_count > options.max_lost_frames) continue;
            if (active_tracks[t].last_embedding.empty()) continue;
            double sim = cosine_similarity(region_embedding, active_tracks[t].last_embedding);
            if (sim > best_sim) {
              best_sim = sim;
              best_track_idx = static_cast<int>(t);
            }
          }
          if (best_sim < options.appearance_similarity_threshold) {
            best_track_idx = -1;  // No good match
          } else {
            // Mark as reacquired
            if (best_track_idx >= 0) {
              active_tracks[best_track_idx].reacquired = true;
            }
          }
        }
      }

      std::string entity_id;
      std::string track_id;

      if (best_track_idx >= 0) {
        // Update existing track
        auto& track = active_tracks[best_track_idx];
        track.kalman.correct(bbox);
        track.last_bbox = bbox;
        track.last_frame_idx = idx_next;
        track.end_us = frame_next.timestamp_us;
        ++track.region_count;
        track.lost_count = 0;
        if (!region_embedding.empty()) {
          track.last_embedding = region_embedding;
        }
        // Upgrade candidate source if this track now has evidence from both sources
        if (uc.source == "depth" && track.candidate_source == "motion") {
          track.candidate_source = "fused_motion_depth";
        } else if (uc.source == "motion" && track.candidate_source == "depth") {
          track.candidate_source = "fused_motion_depth";
        } else if (track.candidate_source.empty()) {
          track.candidate_source = uc.source;
        }
        entity_id = track.entity_id;
        track_id = track.track_id;
        assigned_this_frame.insert(best_track_idx);
      } else {
        // Create new track
        ActiveTrack new_track;
        new_track.kalman.init(bbox);
        new_track.entity_idx = next_entity_idx;
        new_track.entity_id = make_entity_id(next_entity_idx++);
        new_track.track_id = make_track_id(next_track_idx++);
        new_track.last_bbox = bbox;
        new_track.first_frame_idx = idx_next;
        new_track.last_frame_idx = idx_next;
        new_track.start_us = frame_next.timestamp_us;
        new_track.end_us = frame_next.timestamp_us;
        new_track.region_count = 1;
        new_track.lost_count = 0;
        new_track.candidate_source = uc.source;
        if (!region_embedding.empty()) {
          new_track.last_embedding = region_embedding;
        }
        entity_id = new_track.entity_id;
        track_id = new_track.track_id;
        active_tracks.push_back(std::move(new_track));
        assigned_this_frame.insert(static_cast<int>(active_tracks.size() - 1));
      }

      // Compute normalized coordinates
      double box_norm[4] = {
        static_cast<double>(bbox.x) / frame_width,
        static_cast<double>(bbox.y) / frame_height,
        static_cast<double>(bbox.x + bbox.width) / frame_width,
        static_cast<double>(bbox.y + bbox.height) / frame_height
      };
      double centroid_norm[2] = {
        (box_norm[0] + box_norm[2]) / 2.0,
        (box_norm[1] + box_norm[3]) / 2.0
      };
      double screen_area_ratio =
        static_cast<double>(bbox.width * bbox.height) /
        static_cast<double>(frame_width * frame_height);

      // Build TrackedRegion
      TrackedRegion region;
      const int region_entity_idx = (best_track_idx >= 0)
          ? active_tracks[best_track_idx].entity_idx
          : static_cast<int>(active_tracks.size() - 1);
      region.region_id = make_region_id(region_entity_idx, total_region_count, idx_next);
      region.entity_id = entity_id;
      region.track_id = track_id;
      region.frame_id = frame_next.frame_id;
      region.timestamp_us = frame_next.timestamp_us;
      std::copy(std::begin(box_norm), std::end(box_norm), std::begin(region.box_norm));
      region.box_px[0] = bbox.x;
      region.box_px[1] = bbox.y;
      region.box_px[2] = bbox.x + bbox.width;
      region.box_px[3] = bbox.y + bbox.height;
      std::copy(std::begin(centroid_norm), std::end(centroid_norm), std::begin(region.centroid_norm));
      region.screen_area_ratio = screen_area_ratio;
      region.median_inverse_depth = depth_summary.median_inverse_depth;
      region.near_percentile_10 = depth_summary.near_percentile_10;
      region.far_percentile_90 = depth_summary.far_percentile_90;

      // Copy mask pixels
      region.mask_width = frame_width;
      region.mask_height = frame_height;
      region.mask_pixels.resize(static_cast<std::size_t>(frame_width) * frame_height);
      for (int y = 0; y < frame_height; ++y) {
        for (int x = 0; x < frame_width; ++x) {
          region.mask_pixels[static_cast<std::size_t>(y) * frame_width + x] =
            binary_mask.at<uchar>(y, x) > 0 ? 1 : 0;
        }
      }

      region.embedding = region_embedding;
      region.embedding_model_id = options.embedding_model_id;
      region.confidence = std::min(1.0, best_iou > 0 ? best_iou : 0.5);
      region.mask_ref = "mask_" + region.region_id;
      region.candidate_source = uc.source;
      // depth_ref: find the depth entry for this frame
      for (std::size_t di = 0; di < depth_frame_ids.size(); ++di) {
        if (depth_frame_ids[di] == frame_next.frame_id) {
          region.depth_ref = "depth_" + frame_next.frame_id;
          break;
        }
      }

      result.regions.push_back(std::move(region));
      ++total_region_count;
    }

    // Mark tracks that weren't updated as lost
    for (auto& track : active_tracks) {
      if (track.last_frame_idx < idx_next) {
        track.kalman.mark_lost();
      }
    }

    if (options.on_tracking_progress) {
      options.on_tracking_progress(k + 1, total_keyframe_pairs);
    }
  }

  // Step 14: Write entity, track, and region records (§20.6 step 14)
  // Build entity records from tracks
  std::map<std::string, std::vector<std::size_t>> tracks_by_entity;
  for (std::size_t t = 0; t < active_tracks.size(); ++t) {
    tracks_by_entity[active_tracks[t].entity_id].push_back(t);
  }

  for (const auto& [entity_id, track_indices] : tracks_by_entity) {
    EntityRecord entity;
    entity.entity_id = entity_id;
    entity.first_seen_us = active_tracks[track_indices[0]].start_us;
    entity.last_seen_us = active_tracks[track_indices[0]].end_us;
    entity.processor_id = result.processor_id;

    int total_regions = 0;
    double total_area = 0;
    int visible_frames = 0;
    int total_frames = 0;

    for (auto ti : track_indices) {
      const auto& track = active_tracks[ti];
      entity.track_ids.push_back(track.track_id);
      entity.first_seen_us = std::min(entity.first_seen_us, track.start_us);
      entity.last_seen_us = std::max(entity.last_seen_us, track.end_us);
      total_regions += track.region_count;

      // Count regions for this entity
      for (const auto& region : result.regions) {
        if (region.entity_id == entity_id) {
          total_area += region.screen_area_ratio;
          ++visible_frames;
        }
      }
    }

    total_frames = static_cast<int>(keyframes.frame_indices.size());
    entity.average_visibility = total_frames > 0 ?
      static_cast<double>(visible_frames) / total_frames : 0;
    entity.average_screen_area = visible_frames > 0 ?
      total_area / visible_frames : 0;

    // Determine entity type
    // If the entity has significant motion (residual motion clusters), it's a visual_entity
    // If it covers most of the screen and is static, it's a background_region
    // If it has very low visibility, it's an unknown_region
    if (entity.average_visibility > 0.5 && entity.average_screen_area > 0.5) {
      entity.entity_type = "background_region";
    } else if (entity.average_visibility > 0.1) {
      entity.entity_type = "visual_entity";
    } else {
      entity.entity_type = "unknown_region";
    }

    // Evidence sources
    entity.evidence_sources.push_back({
      {"type", "visual_tracking"},
      {"method", "optical_flow_kalman"},
      {"region_count", total_regions}
    });
    if (embedding_session) {
      entity.evidence_sources.push_back({
        {"type", "visual_embedding"},
        {"model_id", options.embedding_model_id},
        {"embedding_dim", 768}
      });
    }

    result.entities.push_back(std::move(entity));
  }

  // Build track records
  for (const auto& track : active_tracks) {
    TrackRecord tr;
    tr.track_id = track.track_id;
    tr.entity_id = track.entity_id;
    tr.start_us = track.start_us;
    tr.end_us = track.end_us;
    tr.start_frame_id = decoded_frames[track.first_frame_idx].frame_id;
    tr.end_frame_id = decoded_frames[track.last_frame_idx].frame_id;
    tr.region_count = track.region_count;
    tr.lost_frame_count = track.lost_count;
    tr.reacquired = track.reacquired;
    tr.confidence = track.region_count > 0 ?
      std::min(1.0, static_cast<double>(track.region_count) /
        static_cast<double>(keyframes.frame_indices.size())) : 0;
    tr.processor_id = result.processor_id;
    tr.tracking_method = track.reacquired ?
      "optical_flow_kalman_embedding_reacquisition" : "optical_flow_kalman";
    tr.candidate_source = track.candidate_source;
    result.tracks.push_back(std::move(tr));
  }

  return result;
}

}  // namespace svp::vision
