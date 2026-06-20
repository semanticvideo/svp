#include "svp/package/spatial_embedding_placeholders.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace svp::package {
namespace {

constexpr const char* kSpatialPlaceholderProcessorId =
    "processor_spatial_placeholder_0001";
constexpr const char* kEmbeddingPlaceholderProcessorId =
    "processor_embedding_placeholder_0001";

void write_empty_file(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
}

void write_text_file(const std::filesystem::path& path,
                     const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output << content;
}

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

nlohmann::json make_spatial_placeholder_processor() {
  return {
      {"id", kSpatialPlaceholderProcessorId},
      {"name", "svp spatial placeholder writer"},
      {"version", "svp-spatial-placeholder-v1"},
      {"input_refs", nlohmann::json::array()},
      {"output_refs", {
          "spatial/depth.index.jsonl",
          "spatial/masks.index.jsonl",
          "spatial/masks.blocks.svpmz"
      }},
      {"model_refs", nlohmann::json::array()},
      {"task_ids", {"task.spatial.placeholder"}},
      {"cache_keys", nlohmann::json::array()},
      {"status", "not_run"},
      {"note", "Depth estimation and mask generation have not been run. "
               "Empty index files and a zero-length mask block stream are "
               "written as honest placeholders. Depth blocks are omitted "
               "because real depth data cannot be produced yet."}
  };
}

nlohmann::json make_embedding_placeholder_processor() {
  return {
      {"id", kEmbeddingPlaceholderProcessorId},
      {"name", "svp embedding placeholder writer"},
      {"version", "svp-embedding-placeholder-v1"},
      {"input_refs", nlohmann::json::array()},
      {"output_refs", {
          "embeddings/embedding_sets.json",
          "embeddings/embeddings.index.jsonl"
      }},
      {"model_refs", nlohmann::json::array()},
      {"task_ids", {"task.embeddings.placeholder"}},
      {"cache_keys", nlohmann::json::array()},
      {"status", "not_run"},
      {"note", "Embedding generation has not been run. An empty embedding "
               "sets file and empty embedding index are written as honest "
               "placeholders. Embedding blocks are omitted because real "
               "model inference cannot be produced yet."}
  };
}

void append_processor_records(
    const std::filesystem::path& processors_path,
    const std::vector<nlohmann::json>& new_processors) {
  std::map<std::string, nlohmann::json> processors_by_id;
  for (const nlohmann::json& processor : read_jsonl(processors_path)) {
    const std::string id = string_value(processor, "id");
    if (!id.empty()) {
      processors_by_id[id] = processor;
    }
  }
  for (const nlohmann::json& processor : new_processors) {
    const std::string id = string_value(processor, "id");
    if (!id.empty()) {
      processors_by_id[id] = processor;
    }
  }
  std::vector<nlohmann::json> all_processors;
  all_processors.reserve(processors_by_id.size());
  for (const auto& [id, processor] : processors_by_id) {
    all_processors.push_back(processor);
  }
  write_jsonl(processors_path, all_processors);
}

}  // namespace

SpatialEmbeddingPlaceholderSummary write_spatial_and_embedding_placeholders(
    const std::filesystem::path& staging_dir) {
  SpatialEmbeddingPlaceholderSummary summary;

  write_empty_file(staging_dir / "spatial" / "depth.index.jsonl");
  summary.depth_index_written = true;

  write_empty_file(staging_dir / "spatial" / "masks.index.jsonl");
  summary.masks_index_written = true;

  write_empty_file(staging_dir / "spatial" / "masks.blocks.svpmz");
  summary.masks_blocks_written = true;

  write_text_file(staging_dir / "embeddings" / "embedding_sets.json",
                  "{\"sets\": []}\n");
  summary.embedding_sets_written = true;

  write_empty_file(staging_dir / "embeddings" / "embeddings.index.jsonl");
  summary.embeddings_index_written = true;

  std::vector<nlohmann::json> placeholder_processors = {
      make_spatial_placeholder_processor(),
      make_embedding_placeholder_processor(),
  };
  append_processor_records(staging_dir / "provenance" / "processors.jsonl",
                           placeholder_processors);
  summary.provenance_records_added = placeholder_processors.size();

  return summary;
}

nlohmann::json spatial_embedding_placeholder_summary_to_json(
    const SpatialEmbeddingPlaceholderSummary& summary) {
  return {
      {"depth_index_written", summary.depth_index_written != 0},
      {"masks_index_written", summary.masks_index_written != 0},
      {"masks_blocks_written", summary.masks_blocks_written != 0},
      {"embedding_sets_written", summary.embedding_sets_written != 0},
      {"embeddings_index_written", summary.embeddings_index_written != 0},
      {"provenance_records_added", summary.provenance_records_added},
  };
}

}  // namespace svp::package
