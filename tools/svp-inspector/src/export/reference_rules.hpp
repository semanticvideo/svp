#pragma once

#include <span>
#include <string_view>
#include <vector>

namespace package_export {

// How the value of a referencing record member is resolved
// (Package_Export_v1.md Section 5).
enum class ReferenceKind {
  frame,       // string frame id -> frame_index, pts_us
  frame_list,  // array of frame ids
  crop,        // string evidence crop id -> exported crop image
  crop_list,   // array of evidence crop ids
  entry_path,  // string package entry path -> exported file
  block,       // block_file + block_offset -> decoded block payload
};

struct ReferenceRule {
  std::string_view layer_entry;
  std::string_view member;
  ReferenceKind kind = ReferenceKind::frame;
};

// Lookup layers and their key members.
inline constexpr std::string_view kFramesLayer = "timeline/frames.jsonl";
inline constexpr std::string_view kFrameIdMember = "id";
inline constexpr std::string_view kFrameIndexMember = "frame_index";
inline constexpr std::string_view kFramePtsMember = "pts_us";
inline constexpr std::string_view kEvidenceCropsLayer =
    "text/evidence_crops.jsonl";
inline constexpr std::string_view kCropIdMember = "crop_id";
inline constexpr std::string_view kCropFilePathMember = "crop_file_path";
inline constexpr std::string_view kBlockOffsetMember = "block_offset";

// The member every resolution is written under in an exported record.
inline constexpr std::string_view kExportMember = "svp_export";

[[nodiscard]] std::string_view to_string(ReferenceKind kind) noexcept;

// The whole registry, in documentation order.
[[nodiscard]] std::span<const ReferenceRule> reference_rules() noexcept;

// Rules for one layer, in registry order.
[[nodiscard]] std::vector<ReferenceRule> rules_for_layer(
    std::string_view layer_entry);

}  // namespace package_export
