#include "working_copy/working_copy_stage.hpp"

#include <cmath>
#include <iostream>

namespace svp::builder::working_copy {

WorkingCopyDecision decide_working_copy(const std::optional<int>& max_resolution,
                                        bool build_decodes_frames,
                                        const svp::media::VideoStreamProbe& primary_video) {
  WorkingCopyDecision decision;
  if (!max_resolution) {
    return decision;
  }
  if (!build_decodes_frames) {
    decision.not_used_reason = "this build decodes no frames";
    return decision;
  }
  decision.geometry = plan_working_copy_geometry(primary_video, *max_resolution);
  if (!decision.geometry) {
    decision.not_used_reason =
        "the video's displayed short side is not longer than --max-resolution";
  }
  return decision;
}

WorkingCopyResult run_working_copy_stage(const WorkingCopyRequest& request,
                                         BuildProgressSink& sink, bool quiet) {
  sink.emit(make_stage_started(ProgressStageId::working_copy));
  WorkingCopyResult made = make_working_copy(request);
  if (made.cancelled) {
    sink.emit(make_stage_failed(ProgressStageId::working_copy, "build interrupted"));
    return made;
  }
  if (made.copy) {
    const WorkingCopy& copy = *made.copy;
    const std::string size = std::to_string(copy.geometry().display_width) + "x" +
                             std::to_string(copy.geometry().display_height);
    sink.emit(make_stage_completed(ProgressStageId::working_copy,
                                   size + " " + copy.encoder().name));
    if (!quiet) {
      std::cerr << "svp-builder: analysing frames from a " << size << " "
                << copy.encoder().name << " working copy (" << copy.fingerprint().size_bytes
                << " bytes, written in " << std::lround(copy.encode_seconds()) << " s)\n";
    }
    return made;
  }
  const std::string warning =
      "no working copy could be made, so frames are decoded from the source: " +
      made.unavailable_reason;
  sink.emit(make_warning(ProgressStageId::working_copy, warning));
  sink.emit(make_stage_completed(ProgressStageId::working_copy, "not used"));
  if (!quiet) {
    std::cerr << "svp-builder: warning: " << warning << "\n";
  }
  return made;
}

}  // namespace svp::builder::working_copy
