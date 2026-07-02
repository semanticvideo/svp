#include "private.hpp"

#include <algorithm>

namespace svp::audio::sherpa_diarization_internal {

std::vector<DiarizationWindowRange> build_diarization_windows(
    std::size_t sample_count) {
  std::vector<DiarizationWindowRange> windows;
  if (sample_count == 0) return windows;

  const std::size_t window_size =
      static_cast<std::size_t>(kDiarizationWindowSamples);
  const std::size_t overlap =
      static_cast<std::size_t>(kDiarizationWindowOverlapSamples);

  std::size_t window_start = 0;
  while (window_start < sample_count) {
    std::size_t accepted_end =
        std::min(window_start + window_size, sample_count);

    std::size_t process_start =
        (window_start == 0)
            ? 0
            : (window_start >= overlap ? window_start - overlap : 0);

    bool is_final = (accepted_end >= sample_count);
    std::size_t process_end =
        is_final ? sample_count
                 : std::min(accepted_end + overlap, sample_count);

    windows.push_back(
        {window_start, accepted_end, process_start, process_end});

    if (is_final) break;
    window_start = accepted_end;
  }

  return windows;
}

bool clip_segment_to_range(SherpaDiarizationSegment& seg,
                           float accepted_start_sec,
                           float accepted_end_sec) {
  float clipped_start = std::max(seg.start_sec, accepted_start_sec);
  float clipped_end = std::min(seg.end_sec, accepted_end_sec);
  if (clipped_end - clipped_start < kMinClippedSegmentDuration) return false;
  seg.start_sec = clipped_start;
  seg.end_sec = clipped_end;
  return true;
}


}  // namespace svp::audio::sherpa_diarization_internal
