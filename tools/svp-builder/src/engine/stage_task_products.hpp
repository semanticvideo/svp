#pragma once

// What one whole-stage task produced, and its encoding as TaskResult outputs
// (plan §4.2: everything that crosses a task boundary is an ArtifactRef).
//
// Output order, which is the task's canonical output order:
//   [0]  role "stage_manifest", application/json, canonical JSON
//          {"directories":[path...],
//           "files":[{"output":i,"path":p}...],
//           "states":[{"name":n,"output":i}...]}
//   [i]  role "staging_file", application/octet-stream: one staged file
//   [j]  role "stage_state", application/octet-stream: one named state blob
// Files and directories are the task's staging capture (staging_scope.hpp);
// states carry in-memory results to dependent tasks and to the final
// assembly (for example the audio foundation record or the canonical frames).

#include "engine/staging_scope.hpp"

#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/frame.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace svp::builder::engine {

struct StageTaskProducts {
  std::vector<StagedEntry> staging;
  // Keyed by state name (a lowercase identifier).
  std::map<std::string, std::vector<std::byte>> states;
};

struct EncodedStageProducts {
  std::vector<svp::exec::ArtifactRef> outputs;
  std::vector<svp::exec::FramePayload> payloads;
};

[[nodiscard]] EncodedStageProducts encode_stage_products(StageTaskProducts products);

// Inverse of encode_stage_products. Throws std::runtime_error when the outputs
// are not a stage encoding (missing or malformed manifest, index out of range,
// payload count mismatch).
[[nodiscard]] StageTaskProducts decode_stage_products(
    std::span<const svp::exec::ArtifactRef> outputs,
    std::span<const svp::exec::FramePayload> payloads);

// JSON state blobs (states are opaque bytes; most are JSON documents).
[[nodiscard]] std::vector<std::byte> json_state_bytes(const nlohmann::json& value);
[[nodiscard]] nlohmann::json parse_json_state(const std::vector<std::byte>& bytes);

}  // namespace svp::builder::engine
