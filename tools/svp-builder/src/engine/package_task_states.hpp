#pragma once

// State names of the package-lane tasks.

namespace svp::builder::engine::package_state {

// The package manifest the entities task stamps (created_utc) and every later
// package stage writes.
inline constexpr const char* kManifest = "package_manifest";
// package_stage_result_to_json of the validation and package write stages.
inline constexpr const char* kPackageResult = "package_result";
// {"bytes": n}: size of the file a publishing task wrote, checked on resume.
inline constexpr const char* kPublished = "published";

}  // namespace svp::builder::engine::package_state
