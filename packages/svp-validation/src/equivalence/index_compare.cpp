#include "equivalence/index_compare.hpp"

#include "equivalence/build_metadata_registry.hpp"
#include "equivalence/derived_digest_registry.hpp"
#include "equivalence/payload_metrics.hpp"
#include "equivalence/sqlite_column_rules.hpp"
#include "sqlite_index_access.hpp"

#include "svp/package/index_logical_rows.hpp"

#include <cmath>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace svp::validation::equivalence {
namespace {

constexpr std::string_view kLayer = "sqlite_logical";
// Report readability only: longer rendered row keys are truncated.
constexpr std::size_t kMaxRenderedKeyChars = 120;
constexpr int kHexBase = 16;

// Canonical scalar kinds of svp-logical-row-stream-v1.
constexpr std::string_view kTextKind = "text";
constexpr std::string_view kBlobKind = "blob";
constexpr std::string_view kIntegerKind = "integer";
constexpr std::string_view kRealKind = "real";

struct TableSnapshot {
  svp::package::LogicalTableDescriptor table;
  std::vector<nlohmann::json> rows;
};

class TableCollector final : public svp::package::LogicalRowStreamVisitor {
 public:
  void on_table(const svp::package::LogicalTableDescriptor& table) override {
    snapshot.table = table;
  }
  void on_row(const svp::package::LogicalTableDescriptor&,
              const nlohmann::json& values) override {
    snapshot.rows.push_back(values);
  }

  TableSnapshot snapshot;
};

TableSnapshot read_table(sqlite3& database, const std::string& name) {
  TableCollector collector;
  svp::package::visit_logical_row_stream(database, {name}, collector);
  return std::move(collector.snapshot);
}

std::string kind_of(const nlohmann::json& value) {
  return value.empty() ? std::string{} : value[0].get<std::string>();
}

std::string hex_decode(std::string_view hex) {
  std::string output;
  output.reserve(hex.size() / 2);
  for (std::size_t index = 0; index + 1 < hex.size(); index += 2) {
    output.push_back(static_cast<char>(
        std::stoi(std::string{hex.substr(index, 2)}, nullptr, kHexBase)));
  }
  return output;
}

std::string hex_encode(std::string_view text) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string output;
  output.reserve(text.size() * 2);
  for (const auto character : text) {
    const auto byte = static_cast<unsigned char>(character);
    output.push_back(kDigits[byte >> 4U]);
    output.push_back(kDigits[byte & 0x0FU]);
  }
  return output;
}

std::vector<std::byte> blob_bytes(const nlohmann::json& value) {
  const auto decoded = hex_decode(value[1].get<std::string>());
  std::vector<std::byte> bytes(decoded.size());
  for (std::size_t index = 0; index < decoded.size(); ++index) {
    bytes[index] = static_cast<std::byte>(decoded[index]);
  }
  return bytes;
}

std::string render_scalar(const nlohmann::json& value) {
  const auto kind = kind_of(value);
  if (kind == kTextKind) {
    return "\"" + hex_decode(value[1].get<std::string>()) + "\"";
  }
  if (kind == kBlobKind) {
    return "blob(" + std::to_string(value[1].get<std::string>().size() / 2) + " bytes)";
  }
  if (kind == kIntegerKind || kind == kRealKind) {
    return value[1].dump();
  }
  return "null";
}

std::optional<double> numeric_value(const nlohmann::json& value) {
  const auto kind = kind_of(value);
  if (kind == kIntegerKind || kind == kRealKind) {
    return value[1].get<double>();
  }
  return std::nullopt;
}

std::optional<std::size_t> column_index(const svp::package::LogicalTableDescriptor& table,
                                        std::string_view column) {
  for (std::size_t index = 0; index < table.column_names.size(); ++index) {
    if (table.column_names[index] == column) {
      return index;
    }
  }
  return std::nullopt;
}

class IndexComparer {
 public:
  IndexComparer(std::string_view entry, bool normalize, EquivalenceLedger& ledger)
      : entry_(entry), normalize_(normalize), ledger_(ledger) {}

  void compare_table(TableSnapshot left, TableSnapshot right);
  void report(EquivalenceOutcome outcome,
              std::string rule,
              std::string location,
              std::string detail,
              std::optional<double> measured = std::nullopt,
              std::optional<double> tolerance = std::nullopt,
              MetricDirection direction = MetricDirection::lower_is_better);

 private:
  void normalize_rows(const std::string& table_name, TableSnapshot& left, TableSnapshot& right);
  void compare_row_pair(const TableSnapshot& table,
                        const std::vector<SqliteColumnRule>& rules,
                        const std::vector<bool>& is_key_column,
                        const nlohmann::json& left,
                        const nlohmann::json& right,
                        const std::string& location);

  std::string entry_;
  bool normalize_ = false;
  EquivalenceLedger& ledger_;
};

void IndexComparer::report(EquivalenceOutcome outcome,
                           std::string rule,
                           std::string location,
                           std::string detail,
                           std::optional<double> measured,
                           std::optional<double> tolerance,
                           MetricDirection direction) {
  ledger_.add(
      EquivalenceFinding{
          .outcome = outcome,
          .entry = entry_,
          .layer = std::string{kLayer},
          .rule = std::move(rule),
          .location = std::move(location),
          .detail = std::move(detail),
          .measured = measured,
          .tolerance = tolerance,
      },
      direction);
}

void IndexComparer::normalize_rows(const std::string& table_name,
                                   TableSnapshot& left,
                                   TableSnapshot& right) {
  for (const auto& field : sqlite_build_metadata_rows()) {
    if (field.table != table_name) {
      continue;
    }
    const auto key_column = column_index(left.table, field.key_column);
    const auto value_column = column_index(left.table, field.value_column);
    if (!key_column || !value_column) {
      continue;
    }
    const nlohmann::json key_value =
        nlohmann::json::array({std::string{kTextKind}, hex_encode(field.key_text)});
    const nlohmann::json placeholder =
        nlohmann::json::array({std::string{kTextKind}, hex_encode(kNormalizedBuildMetadataPlaceholder)});
    std::uint64_t replaced = 0;
    for (auto* snapshot : {&left, &right}) {
      std::uint64_t side_count = 0;
      for (auto& row : snapshot->rows) {
        if (row[*key_column] == key_value) {
          row[*value_column] = placeholder;
          ++side_count;
        }
      }
      replaced = std::max(replaced, side_count);
    }
    if (replaced > 0) {
      auto finding = EquivalenceFinding{
          .outcome = EquivalenceOutcome::normalized_build_metadata,
          .entry = entry_,
          .layer = "build_metadata",
          .rule = "build_metadata:" + table_name + "." + std::string{field.value_column},
          .location = "table " + table_name + " row " + std::string{field.key_column} +
                      "=\"" + std::string{field.key_text} + "\"",
          .detail = std::string{field.reason},
          .occurrences = replaced,
      };
      ledger_.add(std::move(finding));
    }
  }
}

void IndexComparer::compare_table(TableSnapshot left, TableSnapshot right) {
  const auto& name = left.table.name;
  const auto table_location = "table " + name;
  if (left.table.schema_fingerprint != right.table.schema_fingerprint) {
    report(EquivalenceOutcome::not_equivalent, "schema_fingerprint", table_location,
           "normalized schema fingerprint differs");
    return;
  }
  if (left.table.column_names != right.table.column_names) {
    report(EquivalenceOutcome::not_equivalent, "column_names", table_location,
           "column names differ");
    return;
  }
  if (normalize_) {
    normalize_rows(name, left, right);
  }

  std::vector<SqliteColumnRule> rules;
  for (const auto& column : left.table.column_names) {
    rules.push_back(sqlite_column_rule(name, column));
  }

  // Rows are matched by the declared primary key when every key column is
  // exact-governed, otherwise by all exact-class columns; within one key,
  // rows keep canonical stream order.
  std::vector<bool> is_key_column(rules.size(), false);
  bool primary_key_usable = !left.table.primary_key_columns.empty();
  for (const auto& key_column : left.table.primary_key_columns) {
    const auto index = column_index(left.table, key_column);
    primary_key_usable = primary_key_usable && index.has_value() &&
                         rules[*index].column_class == SqliteColumnClass::exact;
    if (index) {
      is_key_column[*index] = true;
    }
  }
  for (std::size_t index = 0; index < rules.size(); ++index) {
    if (!primary_key_usable) {
      is_key_column[index] = rules[index].column_class == SqliteColumnClass::exact;
    }
  }
  const auto row_key = [&](const nlohmann::json& row) {
    nlohmann::json key = nlohmann::json::array();
    for (std::size_t index = 0; index < rules.size(); ++index) {
      if (is_key_column[index]) {
        key.push_back(row[index]);
      }
    }
    return key.dump();
  };
  std::map<std::string, std::pair<std::vector<std::size_t>, std::vector<std::size_t>>> groups;
  for (std::size_t index = 0; index < left.rows.size(); ++index) {
    groups[row_key(left.rows[index])].first.push_back(index);
  }
  for (std::size_t index = 0; index < right.rows.size(); ++index) {
    groups[row_key(right.rows[index])].second.push_back(index);
  }

  for (const auto& [key, members] : groups) {
    const auto& [left_rows, right_rows] = members;
    const auto& sample =
        left_rows.empty() ? right.rows[right_rows.front()] : left.rows[left_rows.front()];
    std::string rendered;
    for (std::size_t index = 0; index < sample.size(); ++index) {
      if (!is_key_column[index]) {
        continue;
      }
      rendered += (rendered.empty() ? "" : ", ") + left.table.column_names[index] + "=" +
                  render_scalar(sample[index]);
    }
    if (rendered.size() > kMaxRenderedKeyChars) {
      rendered.resize(kMaxRenderedKeyChars);
      rendered += "...";
    }
    const auto location = table_location + " row {" + rendered + "}";

    if (left_rows.size() != right_rows.size()) {
      report(EquivalenceOutcome::not_equivalent, "row_set", location,
             "matching exact-column rows: left " + std::to_string(left_rows.size()) +
                 ", right " + std::to_string(right_rows.size()));
      continue;
    }
    for (std::size_t index = 0; index < left_rows.size(); ++index) {
      compare_row_pair(left, rules, is_key_column, left.rows[left_rows[index]],
                       right.rows[right_rows[index]], location);
    }
  }
}

void IndexComparer::compare_row_pair(const TableSnapshot& table,
                                     const std::vector<SqliteColumnRule>& rules,
                                     const std::vector<bool>& is_key_column,
                                     const nlohmann::json& left,
                                     const nlohmann::json& right,
                                     const std::string& location) {
  for (std::size_t index = 0; index < rules.size(); ++index) {
    const auto& rule = rules[index];
    if (is_key_column[index] || left[index] == right[index]) {
      continue;
    }
    const auto& column = table.table.column_names[index];
    const auto column_location = location + " ." + column;
    const auto exact_detail = "left " + render_scalar(left[index]) + ", right " +
                              render_scalar(right[index]);

    switch (rule.column_class) {
      case SqliteColumnClass::numeric_tolerance: {
        const auto left_value = numeric_value(left[index]);
        const auto right_value = numeric_value(right[index]);
        if (!left_value || !right_value) {
          report(EquivalenceOutcome::not_equivalent, std::string{rule.rule_id},
                 column_location, exact_detail + " (null-ness or type differs)");
          break;
        }
        const double difference = std::fabs(*left_value - *right_value);
        report(difference <= rule.tolerance ? EquivalenceOutcome::within_tolerance
                                            : EquivalenceOutcome::not_equivalent,
               std::string{rule.rule_id}, column_location, exact_detail, difference,
               rule.tolerance);
        break;
      }
      case SqliteColumnClass::embedding_vector: {
        if (kind_of(left[index]) != kBlobKind || kind_of(right[index]) != kBlobKind) {
          report(EquivalenceOutcome::not_equivalent, std::string{rule.rule_id},
                 column_location, exact_detail + " (null-ness or type differs)");
          break;
        }
        const auto left_vector = decode_float32_le(blob_bytes(left[index]));
        const auto right_vector = decode_float32_le(blob_bytes(right[index]));
        const double cosine = left_vector.size() == right_vector.size()
                                  ? cosine_similarity(left_vector, right_vector)
                                  : 0.0;
        report(cosine >= rule.tolerance ? EquivalenceOutcome::within_tolerance
                                        : EquivalenceOutcome::not_equivalent,
               std::string{rule.rule_id}, column_location, "float32 vector cosine similarity",
               cosine, rule.tolerance, MetricDirection::higher_is_better);
        break;
      }
      case SqliteColumnClass::derived_block_digest: {
        std::optional<std::string> source;
        for (const auto& digest : sqlite_derived_digest_columns()) {
          if (digest.table != table.table.name || digest.column != column) {
            continue;
          }
          const auto file = column_index(table.table, digest.block_file_column);
          const auto offset = column_index(table.table, digest.block_offset_column);
          if (file && offset && kind_of(left[*file]) == kTextKind &&
              kind_of(left[*offset]) == kIntegerKind) {
            source = block_source_key(hex_decode(left[*file][1].get<std::string>()),
                                      left[*offset][1].get<std::uint64_t>());
          }
        }
        if (!source) {
          report(EquivalenceOutcome::not_equivalent, "derived_digest", column_location,
                 exact_detail + " (referenced block cannot be located)");
          break;
        }
        ledger_.defer_digest(DeferredDigestCheck{
            .entry = entry_,
            .location = column_location,
            .rule = "derived_digest:" + table.table.name + "." + column,
            .source_keys = {*source},
        });
        break;
      }
      case SqliteColumnClass::exact:
        report(EquivalenceOutcome::not_equivalent, "exact", column_location, exact_detail);
        break;
    }
  }
}

}  // namespace

void compare_index_entry(std::string_view entry,
                         const std::filesystem::path& left_package,
                         const std::filesystem::path& right_package,
                         bool normalize_build_metadata,
                         EquivalenceLedger& ledger) {
  IndexComparer comparer{entry, normalize_build_metadata, ledger};
  try {
    const auto left_file = extract_package_entry_to_temp_file(left_package, entry);
    const auto right_file = extract_package_entry_to_temp_file(right_package, entry);
    const auto left_database = open_read_only_database(left_file.path());
    const auto right_database = open_read_only_database(right_file.path());
    const auto left_tables = read_user_table_names(*left_database);
    const auto right_tables = read_user_table_names(*right_database);

    for (const auto& name : left_tables) {
      if (!right_tables.contains(name)) {
        comparer.report(EquivalenceOutcome::not_equivalent, "table_set", "table " + name,
                        "table missing from right package");
        continue;
      }
      comparer.compare_table(read_table(*left_database, name),
                             read_table(*right_database, name));
    }
    for (const auto& name : right_tables) {
      if (!left_tables.contains(name)) {
        comparer.report(EquivalenceOutcome::not_equivalent, "table_set", "table " + name,
                        "table missing from left package");
      }
    }
  } catch (const std::exception& error) {
    comparer.report(EquivalenceOutcome::not_equivalent, "sqlite_readable", "", error.what());
  }
}

}  // namespace svp::validation::equivalence
