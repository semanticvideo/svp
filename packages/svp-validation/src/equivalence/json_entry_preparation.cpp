#include "equivalence/json_entry_preparation.hpp"

#include "equivalence/build_metadata_registry.hpp"
#include "equivalence/derived_digest_registry.hpp"
#include "equivalence/json_pointer_pattern.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <vector>

namespace svp::validation::equivalence {
namespace {

constexpr std::string_view kDigestPlaceholder = "<digest decided by source layer>";

bool is_absolute_host_path(const nlohmann::json& value) {
  if (!value.is_string()) {
    return false;
  }
  const auto& text = value.get_ref<const std::string&>();
  if (!text.empty() && text.front() == '/') {
    return true;
  }
  // Drive-letter paths such as C:\media or C:/media.
  return text.size() >= 3 && std::isalpha(static_cast<unsigned char>(text[0])) != 0 &&
         text[1] == ':' && (text[2] == '\\' || text[2] == '/');
}

bool passes_filter(const nlohmann::json& value, BuildMetadataValueFilter filter) {
  switch (filter) {
    case BuildMetadataValueFilter::any_value:
      return true;
    case BuildMetadataValueFilter::absolute_host_path:
      return is_absolute_host_path(value);
  }
  return false;
}

std::string with_prefix(const std::string& prefix, const std::string& pointer) {
  return prefix.empty() ? pointer : prefix + " " + pointer;
}

// Returns the concrete pointer of the first match (empty when none) and the
// number of values replaced.
std::pair<std::string, std::uint64_t> replace_matches(nlohmann::json& document,
                                                      const JsonBuildMetadataField& field) {
  std::string first;
  std::uint64_t count = 0;
  for_each_pointer_match(document, field.pointer,
                         [&](nlohmann::json& value, const std::string& pointer) {
                           if (!passes_filter(value, field.filter)) {
                             return;
                           }
                           if (first.empty()) {
                             first = pointer;
                           }
                           value = kNormalizedBuildMetadataPlaceholder;
                           ++count;
                         });
  return {first, count};
}

std::optional<std::vector<std::string>> digest_sources(const JsonDerivedDigestField& field,
                                                       const nlohmann::json& record) {
  if (field.kind == DigestSourceKind::package_layers) {
    std::vector<std::string> sources;
    for (const auto source : field.sources) {
      sources.emplace_back(source);
    }
    return sources;
  }
  const auto* block_file = find_pointer(record, "/block_file");
  const auto* block_offset = find_pointer(record, "/block_offset");
  if (block_file == nullptr || block_offset == nullptr || !block_file->is_string() ||
      !block_offset->is_number_unsigned()) {
    return std::nullopt;
  }
  return std::vector<std::string>{
      block_source_key(block_file->get<std::string>(), block_offset->get<std::uint64_t>())};
}

}  // namespace

void normalize_build_metadata(std::string_view entry,
                              nlohmann::json& left,
                              nlohmann::json& right,
                              const std::string& location_prefix,
                              EquivalenceLedger& ledger) {
  for (const auto& field : json_build_metadata_fields()) {
    if (field.entry != entry) {
      continue;
    }
    const auto [left_first, left_count] = replace_matches(left, field);
    const auto [right_first, right_count] = replace_matches(right, field);
    if (left_count == 0 && right_count == 0) {
      continue;
    }
    ledger.add(EquivalenceFinding{
        .outcome = EquivalenceOutcome::normalized_build_metadata,
        .entry = std::string{entry},
        .layer = "build_metadata",
        .rule = "build_metadata:" + std::string{field.pointer},
        .location = with_prefix(location_prefix, left_first.empty() ? right_first : left_first),
        .detail = std::string{field.reason},
        .occurrences = std::max(left_count, right_count),
    });
  }
}

void defer_derived_digests(std::string_view entry,
                           nlohmann::json& left,
                           nlohmann::json& right,
                           const std::string& location_prefix,
                           EquivalenceLedger& ledger) {
  for (const auto& field : json_derived_digest_fields()) {
    if (field.entry != entry) {
      continue;
    }
    const auto* left_value = find_pointer(left, field.pointer);
    const auto* right_value = find_pointer(right, field.pointer);
    if (left_value == nullptr || right_value == nullptr || !left_value->is_string() ||
        !right_value->is_string() || *left_value == *right_value) {
      continue;
    }
    auto sources = digest_sources(field, left);
    if (!sources.has_value()) {
      // Without a locatable source the digest stays exact-governed.
      continue;
    }

    ledger.defer_digest(DeferredDigestCheck{
        .entry = std::string{entry},
        .location = with_prefix(location_prefix, std::string{field.pointer}),
        .rule = "derived_digest:" + std::string{field.pointer},
        .source_keys = std::move(*sources),
    });
    for (auto* document : {&left, &right}) {
      for_each_pointer_match(*document, field.pointer,
                             [](nlohmann::json& value, const std::string&) {
                               value = kDigestPlaceholder;
                             });
    }
  }
}

}  // namespace svp::validation::equivalence
