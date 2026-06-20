#include "svp/package/relationship_provenance_writer.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace svp::package {
namespace {

constexpr const char* kRelationshipProcessorId = "processor_relationship_writer_0001";

std::vector<nlohmann::json> read_jsonl(const std::filesystem::path& path) {
  std::vector<nlohmann::json> records;
  if (!std::filesystem::exists(path)) {
    return records;
  }

  std::ifstream input(path);
  if (!input) {
    return records;
  }

  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) {
      continue;
    }
    try {
      records.push_back(nlohmann::json::parse(line));
    } catch (...) {
    }
  }
  return records;
}

void write_jsonl(const std::filesystem::path& path,
                 const std::vector<nlohmann::json>& records) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  for (const nlohmann::json& record : records) {
    output << record.dump() << "\n";
  }
}

std::string string_value(const nlohmann::json& record, const char* key) {
  const auto iterator = record.find(key);
  if (iterator == record.end() || !iterator->is_string()) {
    return {};
  }
  return iterator->get<std::string>();
}

std::int64_t int_value_or_zero(const nlohmann::json& record, const char* key) {
  const auto iterator = record.find(key);
  if (iterator == record.end() || !iterator->is_number_integer()) {
    return 0;
  }
  return iterator->get<std::int64_t>();
}

double confidence_value_or_one(const nlohmann::json& record) {
  const auto iterator = record.find("confidence");
  if (iterator == record.end() || !iterator->is_number()) {
    return 1.0;
  }
  return iterator->get<double>();
}

nlohmann::json make_relationship(const std::string& id,
                                 const std::string& type,
                                 const std::string& source_id,
                                 const std::string& target_id,
                                 const nlohmann::json& source_record) {
  return {
      {"id", id},
      {"type", type},
      {"source_id", source_id},
      {"target_id", target_id},
      {"start_us", int_value_or_zero(source_record, "start_us")},
      {"end_us", int_value_or_zero(source_record, "end_us")},
      {"evidence", {
          {"source_path", "text/text_regions.jsonl"},
          {"text_region_id", source_id}
      }},
      {"confidence", confidence_value_or_one(source_record)},
      {"processor_id", kRelationshipProcessorId},
  };
}

std::vector<nlohmann::json> build_relationships(const std::filesystem::path& staging_dir) {
  std::vector<nlohmann::json> relationships;
  std::size_t sequence = 1;

  const std::vector<nlohmann::json> text_regions =
      read_jsonl(staging_dir / "text" / "text_regions.jsonl");
  for (const nlohmann::json& region : text_regions) {
    const std::string region_id = string_value(region, "text_region_id");
    if (region_id.empty()) {
      continue;
    }

    const std::string shot_id = string_value(region, "shot_id");
    if (!shot_id.empty()) {
      relationships.push_back(make_relationship(
          "rel_text_region_shot_" + std::to_string(sequence++),
          "appears_in_shot", region_id, shot_id, region));
    }

    const std::string scene_id = string_value(region, "scene_id");
    if (!scene_id.empty()) {
      relationships.push_back(make_relationship(
          "rel_text_region_scene_" + std::to_string(sequence++),
          "appears_in_scene", region_id, scene_id, region));
    }
  }

  std::sort(relationships.begin(), relationships.end(),
            [](const nlohmann::json& lhs, const nlohmann::json& rhs) {
              return lhs.value("id", "") < rhs.value("id", "");
            });
  return relationships;
}

nlohmann::json make_relationship_processor_record() {
  return {
      {"id", kRelationshipProcessorId},
      {"name", "svp package relationship writer"},
      {"version", "svp-package-relationship-writer-v1"},
      {"input_refs", {"text/text_regions.jsonl"}},
      {"output_refs", {"relationships/relationships.jsonl"}},
      {"model_refs", nlohmann::json::array()},
      {"task_ids", {"task.relationships.text_regions"}},
      {"cache_keys", nlohmann::json::array()},
  };
}

RelationshipProvenanceWriteSummary rewrite_processors(
    const std::filesystem::path& staging_dir,
    bool include_relationship_processor) {
  RelationshipProvenanceWriteSummary summary;
  const std::filesystem::path processors_path = staging_dir / "provenance" / "processors.jsonl";
  std::map<std::string, nlohmann::json> processors_by_id;
  std::map<std::string, std::string> canonical_by_id;

  for (const nlohmann::json& processor : read_jsonl(processors_path)) {
    const std::string id = string_value(processor, "id");
    if (id.empty()) {
      continue;
    }

    const std::string canonical = processor.dump();
    const auto existing = canonical_by_id.find(id);
    if (existing != canonical_by_id.end()) {
      ++summary.duplicate_processors_merged;
      if (canonical >= existing->second) {
        continue;
      }
    }

    canonical_by_id[id] = canonical;
    processors_by_id[id] = processor;
  }

  if (include_relationship_processor) {
    const nlohmann::json processor = make_relationship_processor_record();
    processors_by_id[kRelationshipProcessorId] = processor;
    canonical_by_id[kRelationshipProcessorId] = processor.dump();
  }

  std::vector<nlohmann::json> processors;
  processors.reserve(processors_by_id.size());
  for (const auto& [id, processor] : processors_by_id) {
    processors.push_back(processor);
  }

  write_jsonl(processors_path, processors);
  summary.processors_written = processors.size();
  return summary;
}

}  // namespace

RelationshipProvenanceWriteSummary write_relationships_and_provenance(
    const std::filesystem::path& staging_dir) {
  RelationshipProvenanceWriteSummary summary;

  const std::vector<nlohmann::json> relationships = build_relationships(staging_dir);
  write_jsonl(staging_dir / "relationships" / "relationships.jsonl", relationships);
  summary.relationships_written = relationships.size();

  RelationshipProvenanceWriteSummary provenance_summary =
      rewrite_processors(staging_dir, !relationships.empty());
  summary.processors_written = provenance_summary.processors_written;
  summary.duplicate_processors_merged = provenance_summary.duplicate_processors_merged;
  return summary;
}

nlohmann::json relationship_provenance_write_summary_to_json(
    const RelationshipProvenanceWriteSummary& summary) {
  return {
      {"relationships_written", summary.relationships_written},
      {"processors_written", summary.processors_written},
      {"duplicate_processors_merged", summary.duplicate_processors_merged},
  };
}

}  // namespace svp::package
