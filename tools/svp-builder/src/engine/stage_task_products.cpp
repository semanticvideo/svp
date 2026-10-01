#include "engine/stage_task_products.hpp"

#include "svp/exec/canonical_json.hpp"

#include <stdexcept>
#include <string_view>
#include <utility>

namespace svp::builder::engine {
namespace {

constexpr std::string_view kManifestRole = "stage_manifest";
constexpr std::string_view kStagingFileRole = "staging_file";
constexpr std::string_view kStateRole = "stage_state";
constexpr std::string_view kJsonMediaType = "application/json";
constexpr std::string_view kBytesMediaType = "application/octet-stream";

std::vector<std::byte> string_bytes(std::string_view text) {
  const auto* begin = reinterpret_cast<const std::byte*>(text.data());
  return {begin, begin + text.size()};
}

std::string_view bytes_text(std::span<const std::byte> bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

const svp::exec::FramePayload& payload_at(std::span<const svp::exec::FramePayload> payloads,
                                          std::size_t index) {
  if (index == 0 || index >= payloads.size()) {
    throw std::runtime_error("stage manifest names output " + std::to_string(index) +
                             " of " + std::to_string(payloads.size()));
  }
  return payloads[index];
}

}  // namespace

EncodedStageProducts encode_stage_products(StageTaskProducts products) {
  EncodedStageProducts encoded;
  nlohmann::json directories = nlohmann::json::array();
  nlohmann::json files = nlohmann::json::array();
  nlohmann::json states = nlohmann::json::array();
  std::vector<std::pair<std::string, svp::exec::FramePayload>> blobs;

  std::size_t next_output = 1;
  for (StagedEntry& entry : products.staging) {
    if (entry.kind == StagedEntryKind::directory) {
      directories.push_back(entry.relative_path);
      continue;
    }
    files.push_back({{"output", next_output++}, {"path", entry.relative_path}});
    blobs.emplace_back(std::string(kStagingFileRole), std::move(entry.bytes));
  }
  for (auto& [name, bytes] : products.states) {
    states.push_back({{"name", name}, {"output", next_output++}});
    blobs.emplace_back(std::string(kStateRole), std::move(bytes));
  }

  const std::string manifest = svp::exec::encode_canonical_json(
      {{"directories", directories}, {"files", files}, {"states", states}});
  svp::exec::FramePayload manifest_bytes = string_bytes(manifest);
  encoded.outputs.push_back(svp::exec::make_artifact_ref(
      manifest_bytes, std::string(kJsonMediaType), std::string(kManifestRole)));
  encoded.payloads.push_back(std::move(manifest_bytes));
  for (auto& [role, bytes] : blobs) {
    encoded.outputs.push_back(
        svp::exec::make_artifact_ref(bytes, std::string(kBytesMediaType), role));
    encoded.payloads.push_back(std::move(bytes));
  }
  return encoded;
}

StageTaskProducts decode_stage_products(
    std::span<const svp::exec::ArtifactRef> outputs,
    std::span<const svp::exec::FramePayload> payloads) {
  if (outputs.empty() || outputs.size() != payloads.size() ||
      outputs.front().role != kManifestRole) {
    throw std::runtime_error("task outputs are not a stage task encoding");
  }
  const nlohmann::json manifest =
      svp::exec::decode_canonical_json(bytes_text(payloads.front()));

  StageTaskProducts products;
  for (const nlohmann::json& directory : manifest.at("directories")) {
    products.staging.push_back(StagedEntry{.relative_path = directory.get<std::string>(),
                                           .kind = StagedEntryKind::directory,
                                           .bytes = {}});
  }
  for (const nlohmann::json& file : manifest.at("files")) {
    const auto index = file.at("output").get<std::size_t>();
    products.staging.push_back(StagedEntry{.relative_path = file.at("path").get<std::string>(),
                                           .kind = StagedEntryKind::file,
                                           .bytes = payload_at(payloads, index)});
  }
  for (const nlohmann::json& state : manifest.at("states")) {
    const auto index = state.at("output").get<std::size_t>();
    products.states.emplace(state.at("name").get<std::string>(),
                            payload_at(payloads, index));
  }
  return products;
}

std::vector<std::byte> json_state_bytes(const nlohmann::json& value) {
  return string_bytes(value.dump());
}

nlohmann::json parse_json_state(const std::vector<std::byte>& bytes) {
  return nlohmann::json::parse(bytes_text(bytes));
}

}  // namespace svp::builder::engine
