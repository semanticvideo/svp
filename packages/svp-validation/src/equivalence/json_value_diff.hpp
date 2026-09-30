#pragma once

#include "equivalence/canonical_json_compare.hpp"
#include "equivalence/equivalence_ledger.hpp"
#include "equivalence/json_field_rules.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace svp::validation::equivalence {

// Rule-aware structural diff of two canonical JSON values that belong to one
// package entry. Exact everywhere except fields matched by
// find_json_field_rule(); every difference becomes a ledger finding.
class JsonValueDiff {
 public:
  JsonValueDiff(std::string_view entry,
                std::string_view layer,
                const JsonCompareSettings& settings,
                EquivalenceLedger& ledger);

  // `location_prefix` identifies the record, for example "line 12".
  void compare(const nlohmann::json& left,
               const nlohmann::json& right,
               const std::string& location_prefix);

 private:
  void compare_value(const nlohmann::json& left,
                     const nlohmann::json& right,
                     const std::string& pointer);
  void compare_object(const nlohmann::json& left,
                      const nlohmann::json& right,
                      const std::string& pointer);
  void apply_absolute_difference(const JsonFieldRule& rule,
                                 const nlohmann::json& left,
                                 const nlohmann::json& right,
                                 const std::string& pointer);
  void apply_box_iou(const JsonFieldRule& rule,
                     const nlohmann::json& left,
                     const nlohmann::json& right,
                     const std::string& pointer);
  void apply_centroid(const JsonFieldRule& rule,
                      const nlohmann::json& left,
                      const nlohmann::json& right,
                      const std::string& pointer);
  void apply_dominant_bucket(const JsonFieldRule& rule,
                             const nlohmann::json& left,
                             const nlohmann::json& right,
                             const nlohmann::json& left_parent,
                             const nlohmann::json& right_parent,
                             const std::string& pointer);
  void report_exact(const nlohmann::json& left,
                    const nlohmann::json& right,
                    const std::string& pointer,
                    std::string_view rule,
                    std::string detail = {});
  void report_measured(const JsonFieldRule& rule,
                       const std::string& pointer,
                       double measured,
                       bool passes,
                       MetricDirection direction);

  [[nodiscard]] std::string location(const std::string& pointer) const;

  std::string entry_;
  std::string layer_;
  const JsonCompareSettings& settings_;
  EquivalenceLedger& ledger_;
  std::string prefix_;
};

// Short single-line rendering of a JSON value for reports.
[[nodiscard]] std::string render_json_value(const nlohmann::json& value);

}  // namespace svp::validation::equivalence
