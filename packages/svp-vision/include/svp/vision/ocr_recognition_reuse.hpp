#pragma once

#include "svp/vision/color_frame_sampling.hpp"
#include "svp/vision/pp_ocr.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace svp::vision {

// Per-box recognition reuse inside an OCR frame batch.
//
// Detection runs on every OCR sample. Recognition is about 87% of OCR time
// (docs/ocr_performance_notes.md), and most text boxes are the same pixels
// one sample later, so a detected box may take the text and score its match
// from the previous sample was recognized with instead of being recognized
// again:
//
//   - match: the previous sample's box with the highest IoU, which must be
//     above kOcrReuseMinBoxIou (ties go to the earlier box in detector order);
//   - unchanged: on the luma analysis frame (below), at most
//     kOcrReuseMaxChangedPixelFraction of the pixels inside the current box
//     differ by more than kOcrReusePixelDelta from the frame on which the
//     match's text was last recognized;
//   - re-read cap: a box is recognized again after at most
//     kOcrReuseMaxConsecutive reuses in a row.
//
// Output independence from batching: samples form fixed reuse groups of
// kOcrReuseGroupSamples consecutive sample ordinals (group = ordinal /
// kOcrReuseGroupSamples). The first sample of a group recognizes every box,
// and a sample only reuses readings of earlier samples of its own group, so a
// sample's readings depend on its own group alone. Batches start on group
// boundaries (partition_ocr_samples, validated by ocr.frame_batch), so every
// partition of the plan, local or distributed, produces the same records.
// A sample whose previous ordinal was not read (decode miss, failed frame)
// recognizes every box.
//
// What can be missed: a reused box keeps text read up to
// kOcrReuseMaxConsecutive samples earlier, so a change confined to fewer
// than kOcrReuseMaxChangedPixelFraction of the box's analysis pixels (for
// example a one-character edit in a long line) can reach the output that many
// samples late. A changed box, a moved box, and text that appears in a new
// box are always recognized.
//
// Measured on five compliance test videos (1080p and 4K, up to 600 OCR
// samples each; all five SVP packages' text boxes replayed against their frames at
// 640x360 grayscale): reusing whenever a box was unchanged skipped 75% of
// recognitions; forcing a re-read after 2 consecutive reuses skipped 53%.
// Either way all 18 on-screen comments compliance raised from those packages
// were still read; only OCR junk readings were lost (3 with the re-read cap,
// 17 without). The thresholds below are the ones that measurement used.

// Box match between consecutive samples: IoU strictly above this.
inline constexpr double kOcrReuseMinBoxIou = 0.5;
// A pixel changed when its luma (0-255) moved by more than this.
inline constexpr int kOcrReusePixelDelta = 20;
// A box is unchanged when at most this fraction of its pixels changed.
inline constexpr double kOcrReuseMaxChangedPixelFraction = 0.005;
// Longer side of the luma analysis frame the change test runs on: the OCR
// frame is box-averaged by the smallest whole factor that brings it to this
// size or below (1920x1080 -> 640x360, the size of the measurement).
inline constexpr int kOcrReuseAnalysisMaxDimension = 640;
// A box is recognized again after at most this many consecutive reuses.
inline constexpr int kOcrReuseMaxConsecutive = 2;
// Consecutive sample ordinals that share readings; the first of each group
// recognizes everything, which is what bounds consecutive reuses.
inline constexpr std::uint64_t kOcrReuseGroupSamples = kOcrReuseMaxConsecutive + 1;

// True when sample `ordinal` opens a reuse group (a batch may start there).
[[nodiscard]] constexpr bool ocr_reuse_group_starts_at(std::uint64_t ordinal) {
  return ordinal % kOcrReuseGroupSamples == 0;
}

// The grayscale frame the change test compares: Rec. 601 luma of the OCR
// frame, averaged over factor x factor blocks.
struct OcrReuseAnalysisFrame {
  int factor = 1;
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> luma;
};

[[nodiscard]] OcrReuseAnalysisFrame make_ocr_reuse_analysis_frame(
    const ColorRasterFrame& frame);

// True when `box` (OCR frame pixels) changed in at most
// kOcrReuseMaxChangedPixelFraction of its analysis pixels between the two
// frames. A box with no analysis pixels inside the frame counts as changed.
[[nodiscard]] bool ocr_reuse_box_unchanged(const OcrReuseAnalysisFrame& reference,
                                           const OcrReuseAnalysisFrame& current,
                                           const PpOcrTextBox& box);

// The reuse state of one frame batch. Feed it the batch's samples in ordinal
// order; it keeps only the current group's readings and analysis frames.
class OcrRecognitionReuse {
 public:
  // Readings for the boxes detected on sample `ordinal` (decoded as `frame`),
  // one per box in box order: reused where the rule above allows, the rest
  // from one call of `recognize` (with no boxes when every box was reused).
  [[nodiscard]] std::vector<PpOcrDetection> read(std::uint64_t ordinal,
                                                 const ColorRasterFrame& frame,
                                                 std::span<const PpOcrTextBox> boxes,
                                                 const PpOcrRecognizer& recognize);

  // read() for sample `ordinal`, as run_pp_ocr_on_frame takes it. The reader
  // refers to this object, which must outlive it.
  [[nodiscard]] PpOcrBoxReader reader_for(std::uint64_t ordinal);

  // Boxes recognized and boxes that reused a reading, over every read().
  [[nodiscard]] std::size_t recognized_boxes() const noexcept { return recognized_; }
  [[nodiscard]] std::size_t reused_boxes() const noexcept { return reused_; }

 private:
  struct BoxReading {
    PpOcrTextBox box;
    PpOcrDetection reading;
    // Position in the group of the sample whose recognition `reading` holds.
    std::uint64_t recognized_at = 0;
    int consecutive_reuses = 0;
  };

  std::optional<std::uint64_t> last_ordinal_;
  // The current group's analysis frames, by position in the group.
  std::vector<std::optional<OcrReuseAnalysisFrame>> group_frames_;
  // The previous sample's boxes, when it is the sample right before this one.
  std::vector<BoxReading> previous_;
  std::size_t recognized_ = 0;
  std::size_t reused_ = 0;
};

}  // namespace svp::vision
