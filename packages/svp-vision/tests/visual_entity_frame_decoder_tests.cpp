#include "svp/vision/visual_entity_frame_decoder.hpp"

#include <cassert>

int main() {
  svp::media::MediaIngestPlan plan;
  const auto result = svp::vision::decode_visual_entity_window(
      plan, "ffmpeg", 4, 4, {0, 200000, 400000, 450000});
  assert(!result.decoding_attempted);
  assert(result.skipped_reason ==
         "visual entity window timestamps must use a uniform cadence");
  return 0;
}
