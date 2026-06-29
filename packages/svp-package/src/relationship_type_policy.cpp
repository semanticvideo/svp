#include "svp/package/relationship_type_policy.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace svp::package {
namespace {

constexpr std::array<std::string_view, 15> kSupportTypes{{
    "word_spoken_by",
    "word_in_speaker_segment",
    "embedding_source_is",
    "observation_in_region",
    "color_observation_of",
    "depth_for_frame",
    "numeric_value_from_observation",
    "appears_in_frame",
    "region_in_frame",
    "has_mask",
    "track_observation",
    "has_evidence_crop",
    "text_region_overlaps_entity",
    "frame_in_shot",
    "frame_in_scene",
}};

constexpr std::array<std::string_view, 17> kSemanticTypes{{
    "appears_in_shot",
    "appears_in_scene",
    "visible_during_speech",
    "visible_during_word_range",
    "overlaps",
    "near",
    "contains",
    "contained_by",
    "enters_frame",
    "exits_frame",
    "occludes",
    "occluded_by",
    "moves_with",
    "stationary_relative_to_camera",
    "foreground_relative_to",
    "background_relative_to",
    "speaker_active_during_entity_visible",
}};

}  // namespace

RelationshipClass classify_relationship_type(std::string_view type) {
  const auto support_match = std::ranges::find(kSupportTypes, type);
  if (support_match != kSupportTypes.end()) {
    return RelationshipClass::Support;
  }

  const auto semantic_match = std::ranges::find(kSemanticTypes, type);
  if (semantic_match != kSemanticTypes.end()) {
    return RelationshipClass::Semantic;
  }

  return RelationshipClass::Unknown;
}

}  // namespace svp::package
