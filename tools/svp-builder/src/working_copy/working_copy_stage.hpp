#pragma once

// The --max-resolution step of BuildPipeline::run (working_copy.hpp): whether a
// build uses a working copy at all, and the `working_copy` progress stage
// that writes it.

#include "working_copy/working_copy.hpp"
#include "working_copy/working_copy_geometry.hpp"

#include "svp/builder/build_progress.hpp"
#include "svp/media/media_probe.hpp"

#include <memory>
#include <optional>
#include <string>

namespace svp::builder::working_copy {

struct WorkingCopyDecision {
  // Set when the build makes a working copy of this size.
  std::optional<WorkingCopyGeometry> geometry;
  // Otherwise, with --max-resolution given, why not.
  std::string not_used_reason;
};

// A copy is made only when --max-resolution is given, the build decodes frames,
// and the primary video's displayed short side is longer than the bound. In every other
// case the flag changes nothing and the build runs exactly as without it.
[[nodiscard]] WorkingCopyDecision decide_working_copy(
    const std::optional<int>& max_resolution, bool build_decodes_frames,
    const svp::media::VideoStreamProbe& primary_video);

// make_working_copy as the `working_copy` progress stage: started and
// completed events (completed with "not used" and a warning when no copy
// could be made, failed when cancelled), and one line on standard error
// unless `quiet`.
[[nodiscard]] WorkingCopyResult run_working_copy_stage(const WorkingCopyRequest& request,
                                                       BuildProgressSink& sink, bool quiet);

}  // namespace svp::builder::working_copy
