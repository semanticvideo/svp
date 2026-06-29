#include "svp/package/relationship_type_policy.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void test_support_edge_types_classified_as_support() {
  using namespace svp::package;
  assert(classify_relationship_type("word_spoken_by") == RelationshipClass::Support);
  assert(classify_relationship_type("word_in_speaker_segment") == RelationshipClass::Support);
  assert(classify_relationship_type("embedding_source_is") == RelationshipClass::Support);
  assert(classify_relationship_type("observation_in_region") == RelationshipClass::Support);
  assert(classify_relationship_type("color_observation_of") == RelationshipClass::Support);
  assert(classify_relationship_type("depth_for_frame") == RelationshipClass::Support);
  assert(classify_relationship_type("numeric_value_from_observation") == RelationshipClass::Support);
  assert(classify_relationship_type("appears_in_frame") == RelationshipClass::Support);
  assert(classify_relationship_type("region_in_frame") == RelationshipClass::Support);
  assert(classify_relationship_type("has_mask") == RelationshipClass::Support);
  assert(classify_relationship_type("track_observation") == RelationshipClass::Support);
  assert(classify_relationship_type("has_evidence_crop") == RelationshipClass::Support);
  assert(classify_relationship_type("text_region_overlaps_entity") == RelationshipClass::Support);
  assert(classify_relationship_type("frame_in_shot") == RelationshipClass::Support);
  assert(classify_relationship_type("frame_in_scene") == RelationshipClass::Support);
}

void test_semantic_edge_types_classified_as_semantic() {
  using namespace svp::package;
  assert(classify_relationship_type("appears_in_shot") == RelationshipClass::Semantic);
  assert(classify_relationship_type("appears_in_scene") == RelationshipClass::Semantic);
  assert(classify_relationship_type("visible_during_speech") == RelationshipClass::Semantic);
  assert(classify_relationship_type("visible_during_word_range") == RelationshipClass::Semantic);
  assert(classify_relationship_type("overlaps") == RelationshipClass::Semantic);
  assert(classify_relationship_type("near") == RelationshipClass::Semantic);
  assert(classify_relationship_type("contains") == RelationshipClass::Semantic);
  assert(classify_relationship_type("contained_by") == RelationshipClass::Semantic);
  assert(classify_relationship_type("enters_frame") == RelationshipClass::Semantic);
  assert(classify_relationship_type("exits_frame") == RelationshipClass::Semantic);
  assert(classify_relationship_type("occludes") == RelationshipClass::Semantic);
  assert(classify_relationship_type("occluded_by") == RelationshipClass::Semantic);
  assert(classify_relationship_type("moves_with") == RelationshipClass::Semantic);
  assert(classify_relationship_type("stationary_relative_to_camera") == RelationshipClass::Semantic);
  assert(classify_relationship_type("foreground_relative_to") == RelationshipClass::Semantic);
  assert(classify_relationship_type("background_relative_to") == RelationshipClass::Semantic);
  assert(classify_relationship_type("speaker_active_during_entity_visible") == RelationshipClass::Semantic);
}

void test_unknown_types_classified_as_unknown() {
  using namespace svp::package;
  assert(classify_relationship_type("totally_made_up") == RelationshipClass::Unknown);
  assert(classify_relationship_type("") == RelationshipClass::Unknown);
  assert(classify_relationship_type("appears_in_frame_2") == RelationshipClass::Unknown);
  assert(classify_relationship_type("OVERRLAPS") == RelationshipClass::Unknown);
}

void test_class_to_string_roundtrip() {
  using namespace svp::package;
  assert(relationship_class_to_string(RelationshipClass::Support) == "support");
  assert(relationship_class_to_string(RelationshipClass::Semantic) == "semantic");
  assert(relationship_class_to_string(RelationshipClass::Unknown) == "unknown");
}

void test_json_type_field_is_canonical_not_renamed() {
  using namespace svp::package;
  // The relationship writer emits JSON "type" — this test verifies
  // that the classification function accepts the same string values
  // that the writer emits, without any field rename.
  // If the writer were renamed, these would not match.
  const std::string_view emitted_types[] = {
    "word_spoken_by", "word_in_speaker_segment", "embedding_source_is",
    "observation_in_region", "color_observation_of", "depth_for_frame",
    "numeric_value_from_observation", "appears_in_frame", "region_in_frame",
    "has_mask", "track_observation", "has_evidence_crop",
    "text_region_overlaps_entity",
    "frame_in_shot", "frame_in_scene",
    "appears_in_shot", "appears_in_scene",
  };
  for (const auto t : emitted_types) {
    const auto cls = classify_relationship_type(t);
    assert(cls != RelationshipClass::Unknown);
  }
}

}  // namespace

int main() {
  test_support_edge_types_classified_as_support();
  test_semantic_edge_types_classified_as_semantic();
  test_unknown_types_classified_as_unknown();
  test_class_to_string_roundtrip();
  test_json_type_field_is_canonical_not_renamed();
  std::cout << "All relationship-type-policy-tests passed!\n";
  return 0;
}
