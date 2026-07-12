#include "svp/vision/visual_entity_window_assembler.hpp"

#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
  if (condition) return;
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

svp::vision::TrackedRegion region(
    std::string entity_id,
    std::int64_t timestamp_us,
    double x0,
    double x1,
    double area) {
  svp::vision::TrackedRegion value;
  value.entity_id = std::move(entity_id);
  value.frame_id = "frame_" + std::to_string(timestamp_us);
  value.timestamp_us = timestamp_us;
  value.box_norm[0] = x0;
  value.box_norm[1] = 0.2;
  value.box_norm[2] = x1;
  value.box_norm[3] = 0.8;
  value.screen_area_ratio = area;
  value.mask_width = 4;
  value.mask_height = 4;
  value.mask_pixels.assign(16, 1);
  value.candidate_source = "motion";
  return value;
}

svp::vision::EntityTrackResult window(
    std::vector<svp::vision::TrackedRegion> regions) {
  svp::vision::EntityTrackResult result;
  result.processor_id = "proc_visual_entity_tracker_0001";
  result.runtime = "opencv";
  result.execution_provider = "cpu";
  result.regions = std::move(regions);
  return result;
}

svp::vision::TrackedRegion detector_region(
    std::string entity_id,
    std::int64_t timestamp_us,
    std::vector<float> embedding) {
  auto value = region(std::move(entity_id), timestamp_us, 0.2, 0.6, 0.24);
  value.candidate_source = "objectness_detector";
  value.embedding = std::move(embedding);
  return value;
}

void test_overlap_preserves_identity_and_deduplicates_regions() {
  svp::vision::VisualEntityWindowAssembler assembler;
  assembler.append_window(
      window({region("local_a", 0, 0.1, 0.4, 0.18),
              region("local_a", 200000, 0.12, 0.42, 0.18),
              region("local_a", 400000, 0.14, 0.44, 0.18)}),
      {0, 200000, 400000}, 0, -1);
  assembler.append_window(
      window({region("different_local_id", 200000, 0.12, 0.42, 0.18),
              region("different_local_id", 400000, 0.14, 0.44, 0.18),
              region("different_local_id", 600000, 0.16, 0.46, 0.18)}),
      {200000, 400000, 600000}, 200000, 400000);

  auto result = assembler.finish();
  check(result.tracker_result.entities.size() == 1,
        "overlap maps the same object to one entity");
  check(result.tracker_result.tracks.size() == 1,
        "overlap maps the same object to one track");
  check(result.tracker_result.regions.size() == 4,
        "overlap observations are emitted once");
  check(result.masks.size() == 4, "retained masks remain aligned to regions");
}

void test_distinct_overlap_regions_do_not_merge() {
  svp::vision::VisualEntityWindowAssembler assembler;
  assembler.append_window(
      window({region("left", 0, 0.05, 0.25, 0.12),
              region("left", 200000, 0.05, 0.25, 0.12),
              region("left", 400000, 0.05, 0.25, 0.12)}),
      {0, 200000, 400000}, 0, -1);
  assembler.append_window(
      window({region("right", 0, 0.70, 0.90, 0.12),
              region("right", 200000, 0.70, 0.90, 0.12),
              region("right", 400000, 0.70, 0.90, 0.12),
              region("right", 600000, 0.70, 0.90, 0.12),
              region("right", 800000, 0.70, 0.90, 0.12),
              region("right", 1000000, 0.70, 0.90, 0.12)}),
      {0, 200000, 400000, 600000, 800000, 1000000}, 0, 400000);

  auto result = assembler.finish();
  check(result.tracker_result.entities.size() == 2,
        "spatially distinct objects remain separate");
}

void test_singletons_and_full_frame_regions_are_suppressed() {
  svp::vision::VisualEntityWindowAssembler assembler;
  assembler.append_window(
      window({region("singleton", 0, 0.1, 0.3, 0.12),
              region("background", 0, 0.0, 1.0, 1.0),
              region("background", 200000, 0.0, 1.0, 1.0)}),
      {0, 200000}, 0, -1);

  auto result = assembler.finish();
  check(result.tracker_result.entities.empty(),
        "unsupported and full-frame candidates are not entities");
  check(result.masks.empty(), "discarded entities do not retain masks");
}

void test_appearance_reacquires_detector_object_after_gap() {
  svp::vision::VisualEntityWindowAssembler assembler;
  assembler.append_window(
      window({detector_region("before", 0, {1.0F, 0.0F}),
              detector_region("before", 200000, {0.99F, 0.01F}),
              detector_region("before", 400000, {})}),
      {0, 200000, 400000}, 0, -1);
  assembler.append_window(
      window({detector_region("after", 2000000, {0.99F, 0.01F}),
              detector_region("after", 2200000, {}),
              detector_region("after", 2400000, {})}),
      {2000000, 2200000, 2400000}, 2000000, 400000);

  const auto result = assembler.finish();
  check(result.tracker_result.entities.size() == 1,
        "strong detector appearance evidence reacquires after a gap");
}

void test_appearance_does_not_merge_distinct_detector_objects() {
  svp::vision::VisualEntityWindowAssembler assembler;
  assembler.append_window(
      window({detector_region("before", 0, {1.0F, 0.0F}),
              detector_region("before", 200000, {0.99F, 0.01F}),
              detector_region("before", 400000, {})}),
      {0, 200000, 400000}, 0, -1);
  assembler.append_window(
      window({detector_region("after", 2000000, {0.0F, 1.0F}),
              detector_region("after", 2200000, {}),
              detector_region("after", 2400000, {})}),
      {2000000, 2200000, 2400000}, 2000000, 400000);

  const auto result = assembler.finish();
  check(result.tracker_result.entities.size() == 2,
        "appearance disagreement keeps detector objects separate");
}

void test_appearance_reconciles_fragments_inside_one_window() {
  svp::vision::VisualEntityWindowAssembler assembler;
  assembler.append_window(
      window({detector_region("before", 0, {1.0F, 0.0F}),
              detector_region("before", 200000, {0.99F, 0.01F}),
              detector_region("before", 400000, {}),
              detector_region("after", 2000000, {0.99F, 0.01F}),
              detector_region("after", 2200000, {1.0F, 0.0F}),
              detector_region("after", 2400000, {})}),
      {0, 200000, 400000, 2000000, 2200000, 2400000}, 0, -1);

  const auto result = assembler.finish();
  check(result.tracker_result.entities.size() == 1,
        "strong bidirectional appearance evidence reconciles local fragments");
}

void test_detector_category_prevents_appearance_merge() {
  svp::vision::VisualEntityWindowAssembler assembler;
  auto before_a = detector_region("before", 0, {1.0F, 0.0F});
  auto before_b = detector_region("before", 200000, {0.99F, 0.01F});
  auto before_c = detector_region("before", 400000, {});
  auto after_a = detector_region("after", 2000000, {0.88F, 0.475F});
  auto after_b = detector_region("after", 2200000, {0.88F, 0.475F});
  auto after_c = detector_region("after", 2400000, {});
  before_a.detector_category_index = 1;
  before_b.detector_category_index = 1;
  before_c.detector_category_index = 1;
  after_a.detector_category_index = 2;
  after_b.detector_category_index = 2;
  after_c.detector_category_index = 2;
  assembler.append_window(
      window({std::move(before_a), std::move(before_b), std::move(before_c),
              std::move(after_a), std::move(after_b), std::move(after_c)}),
      {0, 200000, 400000, 2000000, 2200000, 2400000}, 0, -1);

  const auto result = assembler.finish();
  check(result.tracker_result.entities.size() == 2,
        "detector category disagreement keeps similar fragments separate");
}

void test_local_fragment_scale_disagreement_prevents_merge() {
  svp::vision::VisualEntityWindowAssembler assembler;
  auto after_a = detector_region("after", 2000000, {1.0F, 0.0F});
  auto after_b = detector_region("after", 2200000, {0.99F, 0.01F});
  auto after_c = detector_region("after", 2400000, {});
  after_a.screen_area_ratio = 0.05;
  after_b.screen_area_ratio = 0.05;
  after_c.screen_area_ratio = 0.05;
  assembler.append_window(
      window({detector_region("before", 0, {1.0F, 0.0F}),
              detector_region("before", 200000, {0.99F, 0.01F}),
              detector_region("before", 400000, {}),
              std::move(after_a), std::move(after_b), std::move(after_c)}),
      {0, 200000, 400000, 2000000, 2200000, 2400000}, 0, -1);

  const auto result = assembler.finish();
  check(result.tracker_result.entities.size() == 2,
        "scale disagreement keeps similar local fragments separate");
}

void test_moderate_appearance_does_not_merge_across_immediate_cut() {
  svp::vision::VisualEntityWindowAssembler assembler;
  assembler.append_window(
      window({detector_region("before", 0, {1.0F, 0.0F}),
              detector_region("before", 200000, {0.99F, 0.01F}),
              detector_region("before", 400000, {}),
              detector_region("after", 600000, {0.88F, 0.475F}),
              detector_region("after", 800000, {0.88F, 0.475F}),
              detector_region("after", 1000000, {})}),
      {0, 200000, 400000, 600000, 800000, 1000000}, 0, -1);

  const auto result = assembler.finish();
  check(result.tracker_result.entities.size() == 2,
        "moderate appearance evidence cannot merge adjacent scene objects");
}

void test_motion_group_reacquires_across_bounded_proposal_gap() {
  svp::vision::VisualEntityWindowAssembler assembler;
  auto before_a = region("before", 0, 0.0, 1.0, 1.0);
  auto before_b = region("before", 200000, 0.0, 1.0, 1.0);
  auto before_c = region("before", 400000, 0.0, 1.0, 1.0);
  auto after_a = region("after", 1400000, 0.0, 1.0, 1.0);
  auto after_b = region("after", 1600000, 0.0, 1.0, 1.0);
  auto after_c = region("after", 1800000, 0.0, 1.0, 1.0);
  for (auto* item : {&before_a, &before_b, &before_c,
                     &after_a, &after_b, &after_c}) {
    item->candidate_source = "motion_group";
  }
  assembler.append_window(
      window({std::move(before_a), std::move(before_b), std::move(before_c),
              std::move(after_a), std::move(after_b), std::move(after_c)}),
      {0, 200000, 400000, 1400000, 1600000, 1800000}, 0, -1);

  const auto result = assembler.finish();
  check(result.tracker_result.entities.size() == 1,
        "matching motion groups reacquire across a bounded proposal gap");
}

}  // namespace

int main() {
  test_overlap_preserves_identity_and_deduplicates_regions();
  test_distinct_overlap_regions_do_not_merge();
  test_singletons_and_full_frame_regions_are_suppressed();
  test_appearance_reacquires_detector_object_after_gap();
  test_appearance_does_not_merge_distinct_detector_objects();
  test_appearance_reconciles_fragments_inside_one_window();
  test_detector_category_prevents_appearance_merge();
  test_local_fragment_scale_disagreement_prevents_merge();
  test_moderate_appearance_does_not_merge_across_immediate_cut();
  test_motion_group_reacquires_across_bounded_proposal_gap();
  return failures == 0 ? 0 : 1;
}
