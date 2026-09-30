#include "equivalence/json_value_diff.hpp"

#include "equivalence/geometry_metrics.hpp"

#include <cmath>
#include <optional>
#include <utility>

namespace svp::validation::equivalence {
namespace {

// Report readability only: longer rendered values are truncated.
constexpr std::size_t kMaxRenderedValueChars = 96;
constexpr std::string_view kExactRule = "exact";
constexpr std::size_t kBoxArity = 4;
constexpr std::size_t kPointArity = 2;

std::string child_pointer(const std::string& pointer, std::string_view key) {
  return pointer + "/" + std::string{key};
}

std::optional<Box> as_box(const nlohmann::json& value) {
  if (!value.is_array() || value.size() != kBoxArity) {
    return std::nullopt;
  }
  Box box{};
  for (std::size_t index = 0; index < kBoxArity; ++index) {
    if (!value[index].is_number()) {
      return std::nullopt;
    }
    box[index] = value[index].get<double>();
  }
  return box;
}

bool is_point(const nlohmann::json& value) {
  return value.is_array() && value.size() == kPointArity && value[0].is_number() &&
         value[1].is_number();
}

}  // namespace

std::string render_json_value(const nlohmann::json& value) {
  auto text = value.dump();
  if (text.size() > kMaxRenderedValueChars) {
    text.resize(kMaxRenderedValueChars);
    text += "...";
  }
  return text;
}

JsonValueDiff::JsonValueDiff(std::string_view entry,
                             std::string_view layer,
                             const JsonCompareSettings& settings,
                             EquivalenceLedger& ledger)
    : entry_(entry), layer_(layer), settings_(settings), ledger_(ledger) {}

void JsonValueDiff::compare(const nlohmann::json& left,
                            const nlohmann::json& right,
                            const std::string& location_prefix) {
  prefix_ = location_prefix;
  compare_value(left, right, std::string{});
}

std::string JsonValueDiff::location(const std::string& pointer) const {
  const auto shown = pointer.empty() ? std::string{"/"} : pointer;
  return prefix_.empty() ? shown : prefix_ + " " + shown;
}

void JsonValueDiff::compare_value(const nlohmann::json& left,
                                  const nlohmann::json& right,
                                  const std::string& pointer) {
  if (left.is_object() && right.is_object()) {
    compare_object(left, right, pointer);
    return;
  }
  if (left.is_array() && right.is_array()) {
    if (left.size() != right.size()) {
      report_exact(left, right, pointer, kExactRule,
                   "array length " + std::to_string(left.size()) + " vs " +
                       std::to_string(right.size()));
      return;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
      compare_value(left[index], right[index],
                    child_pointer(pointer, std::to_string(index)));
    }
    return;
  }
  // nlohmann::json equality compares mixed integer/float numbers by value
  // and everything else by type and value.
  if (left != right) {
    report_exact(left, right, pointer, kExactRule);
  }
}

void JsonValueDiff::compare_object(const nlohmann::json& left,
                                   const nlohmann::json& right,
                                   const std::string& pointer) {
  for (auto item = left.begin(); item != left.end(); ++item) {
    const auto key_pointer = child_pointer(pointer, item.key());
    const auto other = right.find(item.key());
    if (other == right.end()) {
      report_exact(item.value(), nullptr, key_pointer, kExactRule,
                   "key missing from right package");
      continue;
    }

    const auto* rule = find_json_field_rule(entry_, item.key());
    if (rule == nullptr || item.value() == *other) {
      compare_value(item.value(), *other, key_pointer);
      continue;
    }

    switch (rule->kind) {
      case JsonFieldRuleKind::absolute_difference:
        apply_absolute_difference(*rule, item.value(), *other, key_pointer);
        break;
      case JsonFieldRuleKind::bounding_box_iou:
        apply_box_iou(*rule, item.value(), *other, key_pointer);
        break;
      case JsonFieldRuleKind::centroid_displacement_px:
        apply_centroid(*rule, item.value(), *other, key_pointer);
        break;
      case JsonFieldRuleKind::dominant_bucket_tie:
        apply_dominant_bucket(*rule, item.value(), *other, left, right, key_pointer);
        break;
    }
  }

  for (auto item = right.begin(); item != right.end(); ++item) {
    if (!left.contains(item.key())) {
      report_exact(nullptr, item.value(), child_pointer(pointer, item.key()), kExactRule,
                   "key missing from left package");
    }
  }
}

void JsonValueDiff::apply_absolute_difference(const JsonFieldRule& rule,
                                              const nlohmann::json& left,
                                              const nlohmann::json& right,
                                              const std::string& pointer) {
  if (left.is_number() && right.is_number()) {
    const double difference = std::fabs(left.get<double>() - right.get<double>());
    if (difference != 0.0) {
      report_measured(rule, pointer, difference, difference <= rule.tolerance,
                      MetricDirection::lower_is_better);
    }
    return;
  }
  if (left.is_array() && right.is_array() && left.size() == right.size()) {
    for (std::size_t index = 0; index < left.size(); ++index) {
      apply_absolute_difference(rule, left[index], right[index],
                                child_pointer(pointer, std::to_string(index)));
    }
    return;
  }
  if (left.is_object() && right.is_object() && left.size() == right.size()) {
    for (auto item = left.begin(); item != left.end(); ++item) {
      const auto other = right.find(item.key());
      if (other == right.end()) {
        report_exact(item.value(), nullptr, child_pointer(pointer, item.key()),
                     rule.rule_id, "key missing from right package");
        continue;
      }
      apply_absolute_difference(rule, item.value(), *other,
                                child_pointer(pointer, item.key()));
    }
    return;
  }
  // Structure, null-ness, or type differs: the rule only tolerates numeric
  // value differences, so this is an exact-governed mismatch.
  if (left != right) {
    report_exact(left, right, pointer, rule.rule_id,
                 "structure or null-ness differs under a numeric tolerance rule");
  }
}

void JsonValueDiff::apply_box_iou(const JsonFieldRule& rule,
                                  const nlohmann::json& left,
                                  const nlohmann::json& right,
                                  const std::string& pointer) {
  const auto left_box = as_box(left);
  const auto right_box = as_box(right);
  if (!left_box || !right_box) {
    report_exact(left, right, pointer, rule.rule_id,
                 "value is not an [x_min, y_min, x_max, y_max] box");
    return;
  }
  const double iou = box_iou(*left_box, *right_box);
  report_measured(rule, pointer, iou, iou >= rule.tolerance,
                  MetricDirection::higher_is_better);
}

void JsonValueDiff::apply_centroid(const JsonFieldRule& rule,
                                   const nlohmann::json& left,
                                   const nlohmann::json& right,
                                   const std::string& pointer) {
  if (!is_point(left) || !is_point(right)) {
    report_exact(left, right, pointer, rule.rule_id, "value is not an [x, y] point");
    return;
  }
  if (!settings_.raster.has_value()) {
    report_exact(left, right, pointer, rule.rule_id,
                 "rule not evaluable without manifest canonical_analysis_raster; "
                 "compared exactly");
    return;
  }
  const double displacement = scaled_point_distance(
      left[0].get<double>(), left[1].get<double>(), right[0].get<double>(),
      right[1].get<double>(), settings_.raster->width, settings_.raster->height);
  report_measured(rule, pointer, displacement, displacement <= rule.tolerance,
                  MetricDirection::lower_is_better);
}

void JsonValueDiff::apply_dominant_bucket(const JsonFieldRule& rule,
                                          const nlohmann::json& left,
                                          const nlohmann::json& right,
                                          const nlohmann::json& left_parent,
                                          const nlohmann::json& right_parent,
                                          const std::string& pointer) {
  constexpr std::string_view kCoverageKey = "bucket_coverage";
  const auto coverage_gap = [&](const nlohmann::json& parent) -> std::optional<double> {
    const auto coverage = parent.find(std::string{kCoverageKey});
    if (!left.is_string() || !right.is_string() || coverage == parent.end() ||
        !coverage->is_object()) {
      return std::nullopt;
    }
    const auto first = coverage->find(left.get<std::string>());
    const auto second = coverage->find(right.get<std::string>());
    if (first == coverage->end() || second == coverage->end() || !first->is_number() ||
        !second->is_number()) {
      return std::nullopt;
    }
    return std::fabs(first->get<double>() - second->get<double>());
  };

  const auto left_gap = coverage_gap(left_parent);
  const auto right_gap = coverage_gap(right_parent);
  if (!left_gap || !right_gap) {
    report_exact(left, right, pointer, rule.rule_id,
                 "dominant buckets differ and bucket_coverage cannot establish a tie");
    return;
  }
  const double gap = std::max(*left_gap, *right_gap);
  report_measured(rule, pointer, gap, gap <= rule.tolerance,
                  MetricDirection::lower_is_better);
}

void JsonValueDiff::report_exact(const nlohmann::json& left,
                                 const nlohmann::json& right,
                                 const std::string& pointer,
                                 std::string_view rule,
                                 std::string detail) {
  auto message = "left " + render_json_value(left) + ", right " + render_json_value(right);
  if (!detail.empty()) {
    message += " (" + detail + ")";
  }
  ledger_.add(EquivalenceFinding{
      .outcome = EquivalenceOutcome::not_equivalent,
      .entry = entry_,
      .layer = layer_,
      .rule = std::string{rule},
      .location = location(pointer),
      .detail = std::move(message),
  });
}

void JsonValueDiff::report_measured(const JsonFieldRule& rule,
                                    const std::string& pointer,
                                    double measured,
                                    bool passes,
                                    MetricDirection direction) {
  ledger_.add(
      EquivalenceFinding{
          .outcome = passes ? EquivalenceOutcome::within_tolerance
                            : EquivalenceOutcome::not_equivalent,
          .entry = entry_,
          .layer = layer_,
          .rule = std::string{rule.rule_id},
          .location = location(pointer),
          .detail = passes ? "within Default Equivalence Profile v1 tolerance"
                           : "exceeds Default Equivalence Profile v1 tolerance",
          .measured = measured,
          .tolerance = rule.tolerance,
      },
      direction);
}

}  // namespace svp::validation::equivalence
