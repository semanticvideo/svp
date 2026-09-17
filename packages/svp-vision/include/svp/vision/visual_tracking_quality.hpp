#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace svp::vision {

enum class VisualTrackingQuality {
  off,
  low,
  medium,
  high,
};

struct VisualTrackingQualityPolicy {
  std::int64_t sample_interval_us;
  std::int64_t window_duration_us;
  std::int64_t window_overlap_us;
  std::int64_t depth_interval_us;
};

inline constexpr VisualTrackingQuality kDefaultVisualTrackingQuality =
    VisualTrackingQuality::medium;
inline constexpr std::string_view kDefaultVisualTrackingQualityName = "medium";

[[nodiscard]] inline constexpr std::string_view visual_tracking_quality_name(
    VisualTrackingQuality quality) noexcept {
  switch (quality) {
    case VisualTrackingQuality::off:
      return "off";
    case VisualTrackingQuality::low:
      return "low";
    case VisualTrackingQuality::medium:
      return "medium";
    case VisualTrackingQuality::high:
      return "high";
  }
  return kDefaultVisualTrackingQualityName;
}

[[nodiscard]] inline constexpr std::optional<VisualTrackingQuality>
parse_visual_tracking_quality(std::string_view value) noexcept {
  if (value == "off") return VisualTrackingQuality::off;
  if (value == "low") return VisualTrackingQuality::low;
  if (value == "medium") return VisualTrackingQuality::medium;
  if (value == "high") return VisualTrackingQuality::high;
  return std::nullopt;
}

[[nodiscard]] inline constexpr bool visual_tracking_enabled(
    VisualTrackingQuality quality) noexcept {
  return quality != VisualTrackingQuality::off;
}

[[nodiscard]] inline constexpr VisualTrackingQualityPolicy
visual_tracking_quality_policy(VisualTrackingQuality quality) noexcept {
  // Enabled quality levels keep the proven bounded 20-second window and
  // one-second identity handoff. Cadence alone owns the coverage tradeoff.
  constexpr std::int64_t kWindowDurationUs = 20'000'000;
  constexpr std::int64_t kWindowOverlapUs = 1'000'000;
  switch (quality) {
    case VisualTrackingQuality::off:
      return {0, 0, 0, 0};
    case VisualTrackingQuality::low:
      return {500'000, kWindowDurationUs, kWindowOverlapUs, 1'000'000};
    case VisualTrackingQuality::medium:
      // 333,333 microseconds is the measured deterministic approximation of
      // three observations per second. Depth remains cadence-aligned.
      return {333'333, kWindowDurationUs, kWindowOverlapUs, 999'999};
    case VisualTrackingQuality::high:
      return {200'000, kWindowDurationUs, kWindowOverlapUs, 1'000'000};
  }
  return {333'333, kWindowDurationUs, kWindowOverlapUs, 999'999};
}

}  // namespace svp::vision
