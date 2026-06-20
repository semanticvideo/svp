#pragma once

#include <cstddef>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace svp::package {

struct RelationshipProvenanceWriteSummary {
  std::size_t relationships_written = 0;
  std::size_t processors_written = 0;
  std::size_t duplicate_processors_merged = 0;
};

[[nodiscard]] RelationshipProvenanceWriteSummary write_relationships_and_provenance(
    const std::filesystem::path& staging_dir);

[[nodiscard]] nlohmann::json relationship_provenance_write_summary_to_json(
    const RelationshipProvenanceWriteSummary& summary);

}  // namespace svp::package
