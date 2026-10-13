#include "svp/vision/ocr_recognition_reuse.hpp"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace svp::vision {
namespace {

// Rec. 601 luma weights in 1/256 units (they sum to 256).
constexpr int kLumaRed = 77;
constexpr int kLumaGreen = 150;
constexpr int kLumaBlue = 29;

int luma(const Srgb8Pixel& pixel) {
  return (kLumaRed * pixel.r + kLumaGreen * pixel.g + kLumaBlue * pixel.b + 128) >> 8;
}

double box_iou(const PpOcrTextBox& a, const PpOcrTextBox& b) {
  const int inter_left = std::max(a.left, b.left);
  const int inter_top = std::max(a.top, b.top);
  const int inter_right = std::min(a.right, b.right);
  const int inter_bottom = std::min(a.bottom, b.bottom);
  if (inter_right <= inter_left || inter_bottom <= inter_top) return 0.0;
  const auto area = [](const PpOcrTextBox& box) {
    return static_cast<double>(std::max(0, box.right - box.left)) *
           static_cast<double>(std::max(0, box.bottom - box.top));
  };
  const double inter = static_cast<double>(inter_right - inter_left) *
                       static_cast<double>(inter_bottom - inter_top);
  const double union_area = area(a) + area(b) - inter;
  return union_area > 0.0 ? inter / union_area : 0.0;
}

// The analysis pixels a frame-pixel box covers: every block it touches.
struct AnalysisRect {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
};

AnalysisRect analysis_rect(const OcrReuseAnalysisFrame& frame, const PpOcrTextBox& box) {
  const int factor = frame.factor;
  const auto floor_div = [factor](int value) {
    return value >= 0 ? value / factor : -((-value + factor - 1) / factor);
  };
  const auto ceil_div = [factor](int value) {
    return value >= 0 ? (value + factor - 1) / factor : -(-value / factor);
  };
  return AnalysisRect{
      .left = std::clamp(floor_div(box.left), 0, frame.width),
      .top = std::clamp(floor_div(box.top), 0, frame.height),
      .right = std::clamp(ceil_div(box.right), 0, frame.width),
      .bottom = std::clamp(ceil_div(box.bottom), 0, frame.height),
  };
}

// A reading of `box` on `frame` that copies `source`'s text and score; the
// box is clipped to the frame as the recognizer clips it.
PpOcrDetection reused_reading(const PpOcrDetection& source,
                              const PpOcrTextBox& box,
                              const ColorRasterFrame& frame) {
  PpOcrDetection reading;
  reading.text = source.text;
  reading.score = source.score;
  reading.reused = true;
  if (!reading.text.empty()) {
    reading.bbox_left = std::max(0, box.left);
    reading.bbox_top = std::max(0, box.top);
    reading.bbox_right = std::min(frame.width, box.right);
    reading.bbox_bottom = std::min(frame.height, box.bottom);
  }
  return reading;
}

}  // namespace

OcrReuseAnalysisFrame make_ocr_reuse_analysis_frame(const ColorRasterFrame& frame) {
  OcrReuseAnalysisFrame analysis;
  if (frame.width <= 0 || frame.height <= 0 ||
      frame.pixels.size() !=
          static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height)) {
    return analysis;
  }
  const int longer = std::max(frame.width, frame.height);
  analysis.factor = std::max(
      1, (longer + kOcrReuseAnalysisMaxDimension - 1) / kOcrReuseAnalysisMaxDimension);
  analysis.width = frame.width / analysis.factor;
  analysis.height = frame.height / analysis.factor;
  analysis.luma.resize(static_cast<std::size_t>(analysis.width) *
                       static_cast<std::size_t>(analysis.height));
  const int factor = analysis.factor;
  const int block = factor * factor;
  for (int y = 0; y < analysis.height; ++y) {
    for (int x = 0; x < analysis.width; ++x) {
      int sum = 0;
      for (int dy = 0; dy < factor; ++dy) {
        const std::size_t row =
            static_cast<std::size_t>(y * factor + dy) * static_cast<std::size_t>(frame.width);
        for (int dx = 0; dx < factor; ++dx) {
          sum += luma(frame.pixels[row + static_cast<std::size_t>(x * factor + dx)]);
        }
      }
      analysis.luma[static_cast<std::size_t>(y) * static_cast<std::size_t>(analysis.width) +
                    static_cast<std::size_t>(x)] =
          static_cast<std::uint8_t>((sum + block / 2) / block);
    }
  }
  return analysis;
}

bool ocr_reuse_box_unchanged(const OcrReuseAnalysisFrame& reference,
                             const OcrReuseAnalysisFrame& current,
                             const PpOcrTextBox& box) {
  if (reference.factor != current.factor || reference.width != current.width ||
      reference.height != current.height) {
    return false;
  }
  const AnalysisRect rect = analysis_rect(current, box);
  if (rect.right <= rect.left || rect.bottom <= rect.top) return false;
  std::size_t changed = 0;
  for (int y = rect.top; y < rect.bottom; ++y) {
    const std::size_t row =
        static_cast<std::size_t>(y) * static_cast<std::size_t>(current.width);
    for (int x = rect.left; x < rect.right; ++x) {
      const std::size_t index = row + static_cast<std::size_t>(x);
      if (std::abs(static_cast<int>(current.luma[index]) -
                   static_cast<int>(reference.luma[index])) > kOcrReusePixelDelta) {
        ++changed;
      }
    }
  }
  const double area = static_cast<double>(rect.right - rect.left) *
                      static_cast<double>(rect.bottom - rect.top);
  return static_cast<double>(changed) <= kOcrReuseMaxChangedPixelFraction * area;
}

std::vector<PpOcrDetection> OcrRecognitionReuse::read(std::uint64_t ordinal,
                                                      const ColorRasterFrame& frame,
                                                      std::span<const PpOcrTextBox> boxes,
                                                      const PpOcrRecognizer& recognize) {
  const std::uint64_t group = ordinal / kOcrReuseGroupSamples;
  const std::uint64_t position = ordinal % kOcrReuseGroupSamples;
  const bool same_group =
      last_ordinal_.has_value() && *last_ordinal_ / kOcrReuseGroupSamples == group;
  if (!same_group) {
    group_frames_.assign(kOcrReuseGroupSamples, std::nullopt);
  }
  if (!same_group || *last_ordinal_ + 1 != ordinal) {
    previous_.clear();
  }

  OcrReuseAnalysisFrame analysis = make_ocr_reuse_analysis_frame(frame);

  // Each earlier reading goes to at most one current box, highest overlap first
  // (ties by box order), so two boxes never both take one box's text.
  struct Candidate {
    double iou;
    std::size_t box;
    std::size_t earlier;
  };
  std::vector<Candidate> candidates;
  for (std::size_t index = 0; index < boxes.size(); ++index) {
    for (std::size_t earlier = 0; earlier < previous_.size(); ++earlier) {
      const double iou = box_iou(previous_[earlier].box, boxes[index]);
      if (iou > kOcrReuseMinBoxIou) candidates.push_back({iou, index, earlier});
    }
  }
  std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
    if (a.iou != b.iou) return a.iou > b.iou;
    return std::tie(a.box, a.earlier) < std::tie(b.box, b.earlier);
  });
  std::vector<const BoxReading*> matches(boxes.size(), nullptr);
  std::vector<bool> taken(previous_.size(), false);
  for (const Candidate& candidate : candidates) {
    if (matches[candidate.box] != nullptr || taken[candidate.earlier]) continue;
    matches[candidate.box] = &previous_[candidate.earlier];
    taken[candidate.earlier] = true;
  }

  std::vector<BoxReading> current(boxes.size());
  std::vector<std::size_t> pending;
  for (std::size_t index = 0; index < boxes.size(); ++index) {
    const PpOcrTextBox& box = boxes[index];
    current[index].box = box;
    const BoxReading* match = matches[index];
    const std::optional<OcrReuseAnalysisFrame>* reference =
        match != nullptr ? &group_frames_[match->recognized_at] : nullptr;
    if (match != nullptr && match->consecutive_reuses < kOcrReuseMaxConsecutive &&
        reference->has_value() && ocr_reuse_box_unchanged(**reference, analysis, box)) {
      current[index].reading = reused_reading(match->reading, box, frame);
      current[index].recognized_at = match->recognized_at;
      current[index].consecutive_reuses = match->consecutive_reuses + 1;
    } else {
      pending.push_back(index);
    }
  }

  if (!pending.empty()) {
    std::vector<PpOcrTextBox> pending_boxes;
    pending_boxes.reserve(pending.size());
    for (const std::size_t index : pending) pending_boxes.push_back(boxes[index]);
    std::vector<PpOcrDetection> recognized = recognize(pending_boxes);
    if (recognized.size() != pending.size()) {
      throw std::logic_error("PP-OCR recognizer returned " +
                             std::to_string(recognized.size()) + " readings for " +
                             std::to_string(pending.size()) + " boxes");
    }
    for (std::size_t slot = 0; slot < pending.size(); ++slot) {
      BoxReading& reading = current[pending[slot]];
      reading.reading = std::move(recognized[slot]);
      reading.reading.reused = false;
      reading.recognized_at = position;
      reading.consecutive_reuses = 0;
    }
  }
  recognized_ += pending.size();
  reused_ += boxes.size() - pending.size();

  std::vector<PpOcrDetection> readings;
  readings.reserve(current.size());
  for (const BoxReading& reading : current) readings.push_back(reading.reading);
  group_frames_[position] = std::move(analysis);
  previous_ = std::move(current);
  last_ordinal_ = ordinal;
  return readings;
}

PpOcrBoxReader OcrRecognitionReuse::reader_for(std::uint64_t ordinal) {
  return [this, ordinal](const ColorRasterFrame& frame, std::span<const PpOcrTextBox> boxes,
                         const PpOcrRecognizer& recognize) {
    return read(ordinal, frame, boxes, recognize);
  };
}

}  // namespace svp::vision
