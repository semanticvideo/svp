#pragma once

#include <span>
#include <string_view>

namespace svp::validation::equivalence {

enum class JsonFieldRuleKind {
  // Numbers, or arrays/objects of numbers compared element by element:
  // |a - b| <= tolerance. null matches only null.
  absolute_difference,
  // [x_min, y_min, x_max, y_max] boxes: IoU >= tolerance.
  bounding_box_iou,
  // Normalized [x, y] points scaled by the canonical analysis raster:
  // Euclidean displacement in pixels <= tolerance.
  centroid_displacement_px,
  // Bucket label that may differ only when the two labels are tied within
  // `tolerance` in both packages' sibling `bucket_coverage` objects.
  dominant_bucket_tie,
};

enum class JsonKeyMatch {
  exact,
  suffix,
};

// One tolerance-governed JSON field of the Default Equivalence Profile v1.
// Every field that no rule matches is exact-governed.
struct JsonFieldRule {
  std::string_view rule_id;
  // Package entry the rule applies to; empty means every JSON/JSONL entry.
  std::string_view entry;
  std::string_view key;
  JsonKeyMatch match = JsonKeyMatch::exact;
  JsonFieldRuleKind kind = JsonFieldRuleKind::absolute_difference;
  double tolerance = 0.0;
};

[[nodiscard]] const JsonFieldRule* find_json_field_rule(std::string_view entry,
                                                        std::string_view key);

// Payload kinds whose Section 5.16.2 rule this comparator does not evaluate.
// They are compared exactly (stricter, never looser) and listed in every
// report.
[[nodiscard]] std::span<const std::string_view> exact_fallback_notes();

}  // namespace svp::validation::equivalence
