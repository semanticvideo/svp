#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace svp::vision {

// Purpose tags recorded on catalog entries by each frame-decoding stage.
inline constexpr const char* kColorFramePurpose = "color";
inline constexpr const char* kCanonicalFramePurpose = "canonical";
inline constexpr const char* kOcrFramePurpose = "ocr";
inline constexpr const char* kVisualEntityTrackingFramePurpose =
    "visual_entity_tracking";

struct FrameCatalogEntry {
  std::string frame_id;
  std::size_t frame_index = 0;
  std::int64_t timestamp_us = 0;
  bool keyframe = false;
  std::set<std::string> purposes;
  // True once a stage has decoded this frame. In open mode every entry is
  // decoded by construction; in plan mode entries start undecoded.
  bool decoded = false;
};

// Thrown when a stage reports a frame whose timestamp is not in the locked
// plan. It means a stage schedule drifted from the frame plan.
class UnplannedFrameError : public std::logic_error {
public:
  using std::logic_error::logic_error;
};

// Assigns package frame IDs (frame_000001, ...) by timestamp.
//
// Open mode (the default): the first registration of a timestamp assigns the
// next ID. IDs therefore follow registration order.
//
// Plan mode: the complete, ordered set of frames is registered up front and
// lock_to_plan() is called before any stage runs. IDs then depend only on the
// plan, never on which stage or task finishes first. register_frame() becomes
// a lookup that marks the frame decoded and throws UnplannedFrameError for a
// timestamp outside the plan.
class FrameCatalog {
public:
  std::string register_frame(std::int64_t timestamp_us,
                             const std::string& purpose,
                             bool keyframe = false);

  // Freezes the frames registered so far as the plan. Irreversible.
  void lock_to_plan();

  [[nodiscard]] bool locked_to_plan() const;

  std::optional<std::size_t> get_frame_index(const std::string& frame_id) const;

  std::optional<std::size_t> get_frame_index(std::int64_t timestamp_us) const;

  // Frames that stages actually decoded, in frame ID order. In open mode this
  // is every registered frame. In plan mode, planned frames that no stage
  // decoded (for example a decode miss) are omitted.
  std::vector<FrameCatalogEntry> entries() const;

  // Every registered frame, decoded or not, in frame ID order.
  const std::vector<FrameCatalogEntry>& planned_entries() const;

  std::size_t size() const;

private:
  std::map<std::int64_t, std::size_t> timestamp_to_index_;
  std::vector<FrameCatalogEntry> entries_;
  bool locked_to_plan_ = false;

  static std::string make_frame_id(std::size_t index);
};

}  // namespace svp::vision
