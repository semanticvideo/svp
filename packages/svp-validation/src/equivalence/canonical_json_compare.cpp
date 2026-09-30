#include "equivalence/canonical_json_compare.hpp"

#include "equivalence/json_entry_preparation.hpp"
#include "equivalence/json_value_diff.hpp"

#include <optional>
#include <vector>

namespace svp::validation::equivalence {
namespace {

struct JsonRecord {
  nlohmann::json value;
  std::size_t line = 0;
};

// JSON: one record. JSONL: one record per non-blank line; a trailing "\r"
// is dropped so CRLF and LF line endings compare equal.
std::optional<std::vector<JsonRecord>> parse_records(const std::string& bytes,
                                                     JsonEntryKind kind) {
  std::vector<JsonRecord> records;
  try {
    if (kind == JsonEntryKind::json) {
      records.push_back(JsonRecord{.value = nlohmann::json::parse(bytes), .line = 0});
      return records;
    }
    std::size_t start = 0;
    std::size_t line = 0;
    while (start < bytes.size()) {
      auto end = bytes.find('\n', start);
      if (end == std::string::npos) {
        end = bytes.size();
      }
      ++line;
      std::string_view text{bytes.data() + start, end - start};
      if (!text.empty() && text.back() == '\r') {
        text.remove_suffix(1);
      }
      if (!text.empty()) {
        records.push_back(JsonRecord{.value = nlohmann::json::parse(text), .line = line});
      }
      start = end + 1;
    }
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
  return records;
}

bool canonically_identical(const std::vector<JsonRecord>& left,
                           const std::vector<JsonRecord>& right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    // nlohmann::json keeps object keys sorted, so dump() is key-order
    // independent; it rejects invalid UTF-8 by throwing during parse/dump.
    if (left[index].value.dump() != right[index].value.dump()) {
      return false;
    }
  }
  return true;
}

std::string record_prefix(JsonEntryKind kind, const JsonRecord& record) {
  return kind == JsonEntryKind::jsonl ? "line " + std::to_string(record.line) : std::string{};
}

}  // namespace

bool compare_json_entry(std::string_view entry,
                        JsonEntryKind kind,
                        const std::string& left_bytes,
                        const std::string& right_bytes,
                        const JsonCompareSettings& settings,
                        EquivalenceLedger& ledger) {
  auto left = parse_records(left_bytes, kind);
  auto right = parse_records(right_bytes, kind);
  if (!left.has_value() || !right.has_value()) {
    return false;
  }

  const std::string layer = kind == JsonEntryKind::json ? "canonical_json" : "canonical_jsonl";
  if (canonically_identical(*left, *right)) {
    ledger.add(EquivalenceFinding{
        .outcome = EquivalenceOutcome::canonicalization_only,
        .entry = std::string{entry},
        .layer = layer,
        .rule = "canonical_json_jsonl",
        .detail = "Bytes differ; canonicalized content is identical.",
    });
    return true;
  }

  const auto shared = std::min(left->size(), right->size());
  JsonValueDiff diff{entry, layer, settings, ledger};
  for (std::size_t index = 0; index < shared; ++index) {
    auto& left_record = (*left)[index];
    auto& right_record = (*right)[index];
    const auto prefix = record_prefix(kind, left_record);
    if (settings.normalize_build_metadata) {
      normalize_build_metadata(entry, left_record.value, right_record.value, prefix,
                               ledger);
    }
    defer_derived_digests(entry, left_record.value, right_record.value, prefix, ledger);
    diff.compare(left_record.value, right_record.value, prefix);
  }

  if (left->size() != right->size()) {
    ledger.add(EquivalenceFinding{
        .outcome = EquivalenceOutcome::not_equivalent,
        .entry = std::string{entry},
        .layer = layer,
        .rule = "record_count",
        .location = "record " + std::to_string(shared + 1),
        .detail = "left has " + std::to_string(left->size()) + " records, right has " +
                  std::to_string(right->size()),
    });
  }
  return true;
}

}  // namespace svp::validation::equivalence
