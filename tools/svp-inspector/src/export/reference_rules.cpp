#include "reference_rules.hpp"

#include <array>

namespace package_export {
namespace {

// Every record member the export resolves. Members not listed here are never
// touched. The table is mirrored in Package_Export_v1.md Section 5.
constexpr std::array<ReferenceRule, 15> kReferenceRules{{
    {"text/text_observations.jsonl", "source_frame_ids",
     ReferenceKind::frame_list},
    {"text/text_observations.jsonl", "evidence_crop_refs",
     ReferenceKind::crop_list},
    {"text/evidence_crops.jsonl", "source_frame_id", ReferenceKind::frame},
    {"text/evidence_crops.jsonl", "crop_file_path", ReferenceKind::entry_path},
    {"colors/color_observations.jsonl", "frame_ids",
     ReferenceKind::frame_list},
    {"timeline/shots.jsonl", "start_frame_id", ReferenceKind::frame},
    {"timeline/shots.jsonl", "end_frame_id", ReferenceKind::frame},
    {"entities/entity_tracks.jsonl", "start_frame_id", ReferenceKind::frame},
    {"entities/entity_tracks.jsonl", "end_frame_id", ReferenceKind::frame},
    {"spatial/regions.jsonl", "frame_id", ReferenceKind::frame},
    {"spatial/depth.index.jsonl", "frame_id", ReferenceKind::frame},
    {"spatial/depth.index.jsonl", "block_file", ReferenceKind::block},
    {"spatial/masks.index.jsonl", "frame_id", ReferenceKind::frame},
    {"spatial/masks.index.jsonl", "block_file", ReferenceKind::block},
    {"embeddings/embeddings.index.jsonl", "block_file", ReferenceKind::block},
}};

}  // namespace

std::string_view to_string(ReferenceKind kind) noexcept {
  switch (kind) {
    case ReferenceKind::frame:
      return "frame";
    case ReferenceKind::frame_list:
      return "frame_list";
    case ReferenceKind::crop:
      return "crop";
    case ReferenceKind::crop_list:
      return "crop_list";
    case ReferenceKind::entry_path:
      return "entry_path";
    case ReferenceKind::block:
      return "block";
  }
  return "frame";
}

std::span<const ReferenceRule> reference_rules() noexcept {
  return kReferenceRules;
}

std::vector<ReferenceRule> rules_for_layer(std::string_view layer_entry) {
  std::vector<ReferenceRule> rules;
  for (const auto& rule : reference_rules()) {
    if (rule.layer_entry == layer_entry) {
      rules.push_back(rule);
    }
  }
  return rules;
}

}  // namespace package_export
