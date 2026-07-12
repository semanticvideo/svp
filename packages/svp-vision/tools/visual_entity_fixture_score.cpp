#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace {

struct OutputEntity {
  std::string id;
  std::string type;
  std::int64_t start_us = 0;
  std::int64_t end_us = 0;
};

struct OutputRegion {
  std::string entity_id;
  std::string candidate_source;
  std::int64_t pts_us = 0;
  double box[4] = {0.0, 0.0, 0.0, 0.0};
};

constexpr std::int64_t kTrackedCheckpointToleranceUs = 100000;
// Dynamic groups are motion-supported aggregates rather than continuously
// measured object boxes. Permit one missed 5 Hz proposal while still
// requiring nearby spatial evidence from the same tracked group.
constexpr std::int64_t kDynamicGroupCheckpointToleranceUs = 300000;

bool reference_accepts_entity(
    const std::string& reference_kind,
    const OutputEntity& entity) {
  if (reference_kind == "dynamic_group") {
    return entity.type == "dynamic_group";
  }
  if (reference_kind == "persistent_entity") {
    return entity.type != "dynamic_group";
  }
  return true;
}

bool reference_accepts_region(
    const std::string& reference_kind,
    const OutputRegion& region) {
  if (reference_kind == "dynamic_group") {
    return region.candidate_source == "motion_group";
  }
  if (reference_kind == "persistent_entity") {
    return region.candidate_source != "motion_group";
  }
  return true;
}

double box_iou(const double left[4], const double right[4]) {
  const double x0 = std::max(left[0], right[0]);
  const double y0 = std::max(left[1], right[1]);
  const double x1 = std::min(left[2], right[2]);
  const double y1 = std::min(left[3], right[3]);
  const double intersection =
      std::max(0.0, x1 - x0) * std::max(0.0, y1 - y0);
  const double left_area =
      std::max(0.0, left[2] - left[0]) * std::max(0.0, left[3] - left[1]);
  const double right_area =
      std::max(0.0, right[2] - right[0]) * std::max(0.0, right[3] - right[1]);
  const double union_area = left_area + right_area - intersection;
  return union_area > 0.0 ? intersection / union_area : 0.0;
}

double interval_coverage(
    std::int64_t expected_start,
    std::int64_t expected_end,
    const OutputEntity& entity) {
  const std::int64_t intersection =
      std::max<std::int64_t>(0, std::min(expected_end, entity.end_us) -
                                   std::max(expected_start, entity.start_us));
  const std::int64_t duration = expected_end - expected_start;
  return duration > 0
      ? static_cast<double>(intersection) / static_cast<double>(duration)
      : 0.0;
}

nlohmann::json load_json(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("failed to open " + path.string());
  nlohmann::json value;
  input >> value;
  return value;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: svp-visual-entity-fixture-score "
                 "<diagnostic-json> <north-star-json> <video-name>\n";
    return 2;
  }

  try {
    const auto diagnostic = load_json(argv[1]);
    const auto north_star = load_json(argv[2]);
    const std::string video_name = argv[3];
    if (!north_star.contains(video_name)) {
      throw std::runtime_error("north star has no entry for " + video_name);
    }

    std::vector<OutputEntity> output_entities;
    for (const auto& entity : diagnostic.at("entities")) {
      output_entities.push_back({
          entity.at("id").get<std::string>(),
          entity.at("entity_type").get<std::string>(),
          entity.at("first_seen_us").get<std::int64_t>(),
          entity.at("last_seen_us").get<std::int64_t>()});
    }
    std::vector<OutputRegion> output_regions;
    for (const auto& region : diagnostic.at("regions")) {
      OutputRegion output_region;
      output_region.entity_id = region.at("entity_id").get<std::string>();
      output_region.candidate_source =
          region.at("candidate_source").get<std::string>();
      output_region.pts_us = region.at("pts_us").get<std::int64_t>();
      for (std::size_t index = 0; index < 4; ++index) {
        output_region.box[index] =
            region.at("box_norm").at(index).get<double>();
      }
      output_regions.push_back(std::move(output_region));
    }

    nlohmann::json reference_scores = nlohmann::json::array();
    double total_coverage = 0.0;
    std::size_t interval_count = 0;
    std::size_t repeated_reference_count = 0;
    std::size_t identity_consistent_count = 0;
    double total_checkpoint_iou = 0.0;
    std::size_t checkpoint_count = 0;
    std::size_t tracked_reference_count = 0;

    for (const auto& reference : north_star.at(video_name).at("references")) {
      const bool entity_tracking = reference.value("entity_tracking", true);
      const std::string reference_kind = reference.at("kind").get<std::string>();
      nlohmann::json interval_scores = nlohmann::json::array();
      std::vector<std::string> selected_ids;
      for (const auto& interval : reference.at("intervals")) {
        const std::int64_t start_us = interval.at(0).get<std::int64_t>();
        const std::int64_t end_us = interval.at(1).get<std::int64_t>();
        double best_coverage = 0.0;
        std::string best_entity_id;
        for (const auto& entity : output_entities) {
          if (!reference_accepts_entity(reference_kind, entity)) continue;
          const double coverage = interval_coverage(start_us, end_us, entity);
          if (coverage > best_coverage ||
              (coverage > 0.0 && coverage == best_coverage && !entity.id.empty() &&
               (best_entity_id.empty() || entity.id < best_entity_id))) {
            best_coverage = coverage;
            best_entity_id = entity.id;
          }
        }
        selected_ids.push_back(best_entity_id);
        interval_scores.push_back({
            {"start_us", start_us},
            {"end_us", end_us},
            {"coverage", best_coverage},
            {"entity_id", best_entity_id}});
      }

      nlohmann::json checkpoint_scores = nlohmann::json::array();
      std::vector<std::string> checkpoint_entity_ids;
      std::vector<std::int64_t> checkpoint_timestamps_us;
      std::vector<double> checkpoint_ious;
      if (entity_tracking) {
        ++tracked_reference_count;
        for (const auto& checkpoint : reference.value(
                 "checkpoints", nlohmann::json::array())) {
          const std::int64_t pts_us = checkpoint.at("pts_us").get<std::int64_t>();
          double expected_box[4];
          for (std::size_t index = 0; index < 4; ++index) {
            expected_box[index] =
                checkpoint.at("box_norm").at(index).get<double>();
          }
          double best_iou = 0.0;
          std::string best_entity_id;
          const auto checkpoint_tolerance_us =
              reference_kind == "dynamic_group"
              ? kDynamicGroupCheckpointToleranceUs
              : kTrackedCheckpointToleranceUs;
          for (const auto& region : output_regions) {
            if (std::abs(region.pts_us - pts_us) >
                checkpoint_tolerance_us) {
              continue;
            }
            if (!reference_accepts_region(reference_kind, region)) continue;
            const double iou = box_iou(expected_box, region.box);
            if (iou > best_iou ||
                (iou > 0.0 && iou == best_iou &&
                 (best_entity_id.empty() || region.entity_id < best_entity_id))) {
              best_iou = iou;
              best_entity_id = region.entity_id;
            }
          }
          checkpoint_entity_ids.push_back(best_entity_id);
          checkpoint_timestamps_us.push_back(pts_us);
          checkpoint_ious.push_back(best_iou);
          total_checkpoint_iou += best_iou;
          ++checkpoint_count;
          checkpoint_scores.push_back({
              {"pts_us", pts_us},
              {"iou", best_iou},
              {"entity_id", best_entity_id}});
        }
      }

      if (!checkpoint_timestamps_us.empty()) {
        interval_scores = nlohmann::json::array();
        selected_ids.clear();
        for (const auto& interval : reference.at("intervals")) {
          const std::int64_t start_us = interval.at(0).get<std::int64_t>();
          const std::int64_t end_us = interval.at(1).get<std::int64_t>();
          std::string anchored_entity_id;
          double anchored_iou = -1.0;
          for (std::size_t checkpoint_index = 0;
               checkpoint_index < checkpoint_timestamps_us.size();
               ++checkpoint_index) {
            if (checkpoint_timestamps_us[checkpoint_index] < start_us ||
                checkpoint_timestamps_us[checkpoint_index] > end_us) {
              continue;
            }
            if (checkpoint_ious[checkpoint_index] > anchored_iou) {
              anchored_iou = checkpoint_ious[checkpoint_index];
              anchored_entity_id = checkpoint_entity_ids[checkpoint_index];
            }
          }
          double coverage = 0.0;
          if (!anchored_entity_id.empty()) {
            const auto entity = std::find_if(
                output_entities.begin(), output_entities.end(),
                [&](const OutputEntity& candidate) {
                  return candidate.id == anchored_entity_id;
                });
            if (entity != output_entities.end()) {
              coverage = interval_coverage(start_us, end_us, *entity);
            }
          }
          selected_ids.push_back(anchored_entity_id);
          interval_scores.push_back({
              {"start_us", start_us},
              {"end_us", end_us},
              {"coverage", coverage},
              {"entity_id", anchored_entity_id}});
        }
      }

      if (entity_tracking) {
        for (const auto& interval_score : interval_scores) {
          total_coverage += interval_score.at("coverage").get<double>();
          ++interval_count;
        }
      }

      bool identity_consistent = true;
      const auto& identity_ids = checkpoint_entity_ids.size() > 1
          ? checkpoint_entity_ids
          : selected_ids;
      if (entity_tracking && identity_ids.size() > 1) {
        ++repeated_reference_count;
        identity_consistent =
            std::all_of(identity_ids.begin() + 1, identity_ids.end(),
                        [&](const std::string& id) {
                          return !identity_ids.front().empty() &&
                                 id == identity_ids.front();
                        });
        if (identity_consistent) ++identity_consistent_count;
      }
      reference_scores.push_back({
          {"id", reference.at("id")},
          {"kind", reference.at("kind")},
          {"entity_tracking", entity_tracking},
          {"identity_consistent", identity_consistent},
          {"intervals", std::move(interval_scores)},
          {"checkpoints", std::move(checkpoint_scores)}});
    }

    const double mean_coverage = interval_count > 0
        ? total_coverage / static_cast<double>(interval_count)
        : 0.0;
    const double identity_consistency = repeated_reference_count > 0
        ? static_cast<double>(identity_consistent_count) /
              static_cast<double>(repeated_reference_count)
        : 1.0;
    const double mean_checkpoint_iou = checkpoint_count > 0
        ? total_checkpoint_iou / static_cast<double>(checkpoint_count)
        : 0.0;
    nlohmann::json result = {
        {"video", video_name},
        {"mean_interval_coverage", mean_coverage},
        {"mean_checkpoint_iou", mean_checkpoint_iou},
        {"repeated_identity_consistency", identity_consistency},
        {"output_entity_count", output_entities.size()},
        {"reference_count",
         north_star.at(video_name).at("references").size()},
        {"tracked_reference_count", tracked_reference_count},
        {"references", std::move(reference_scores)}};
    std::cout << result.dump(2) << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
