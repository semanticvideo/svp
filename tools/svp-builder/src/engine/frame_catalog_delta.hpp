#pragma once

// Frame registrations a task made on its own copy of the planned frame
// catalog. Frame IDs are fixed at plan time (#159), but stages still mark the
// frames they decode (purpose, keyframe, decoded), and the timeline rewrite
// lists only decoded frames. Each decoding task therefore reports what it
// registered, and the task that rewrites frames.jsonl replays those reports
// on a fresh copy of the plan. Registration only inserts purposes, ORs the
// keyframe flag, and sets decoded, so replay order does not matter and a
// resumed build reproduces the catalog of an uninterrupted one.

#include "svp/vision/frame_catalog.hpp"

#include <nlohmann/json.hpp>

namespace svp::builder::engine {

// JSON array of {"keyframe","purposes":[...],"timestamp_us"} for every frame
// decoded in `after` (a copy of the locked plan the task used).
[[nodiscard]] nlohmann::json frame_catalog_delta(const svp::vision::FrameCatalog& after);

// Replays a delta onto `catalog` (a locked copy of the same plan). Throws
// svp::vision::UnplannedFrameError for a frame outside the plan.
void apply_frame_catalog_delta(svp::vision::FrameCatalog& catalog,
                               const nlohmann::json& delta);

}  // namespace svp::builder::engine
