#include "svp/package/spatial_embedding_placeholders.hpp"

#include "svp/vision/depth_generation.hpp"
#include "svp/vision/embedding_generation.hpp"

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
          "spatial/depth.blocks.svpdz",
          "spatial/masks.index.jsonl",
          "spatial/masks.blocks.svpmz"
      }},
      {"model_refs", nlohmann::json::array()},
      {"task_ids", {"task.spatial.placeholder"}},
      {"cache_keys", nlohmann::json::array()},
      {"status", "not_run"},
      {"note", "Depth estimation and mask generation have not been run. "
               "Empty index files, a zero-length mask block stream, and a "
               "zero-length depth block stream are written as honest "
               "placeholders. The validator requires real depth blocks "
               "with frame ranges; the empty depth block stream will be "
               "reported as invalid until real depth estimation is run. "
               "Depth requires Depth Anything V2 Small via ONNX Runtime, "
               "which is not configured in this build."}
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
          "embeddings/embeddings.index.jsonl",
          "embeddings/embeddings.blocks.svpez"
      }},
      {"model_refs", nlohmann::json::array()},
      {"task_ids", {"task.embeddings.placeholder"}},
      {"cache_keys", nlohmann::json::array()},
      {"status", "not_run"},
      {"note", "Embedding generation has not been run. An empty embedding "
               "sets file, empty embedding index, and a zero-length "
               "embedding block stream are written as honest placeholders. "
               "The validator requires real embedding blocks with model "
               "output; the empty embedding block stream will be reported "
               "as invalid until real model inference is run. Embeddings "
               "require Nomic text/vision models via ONNX Runtime, which "
               "is not configured in this build."}
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
    const std::filesystem::path& staging_dir,
    bool model_runtime_available,
    const nlohmann::json& media_plan_json,
    const std::filesystem::path& model_cache_root) {
  SpatialEmbeddingPlaceholderSummary summary;
  summary.model_runtime_available = model_runtime_available;

  if (model_runtime_available) {
    std::uint32_t raster_w = 640;
    std::uint32_t raster_h = 360;
    if (!media_plan_json.empty() &&
        media_plan_json.contains("canonical_raster")) {
      const auto& raster = media_plan_json["canonical_raster"];
      if (raster.contains("width") && raster["width"].is_number()) {
        raster_w = raster["width"].get<std::uint32_t>();
      }
      if (raster.contains("height") && raster["height"].is_number()) {
        raster_h = raster["height"].get<std::uint32_t>();
      }
    }

    svp::vision::DepthGenerationOptions depth_opts;
    depth_opts.model_cache_root = model_cache_root;
    depth_opts.raster_width = raster_w;
    depth_opts.raster_height = raster_h;

    svp::vision::DepthGenerationResult depth_result;
    try {
      depth_result = svp::vision::generate_depth_blocks(
          depth_opts, staging_dir);
    } catch (const std::exception& e) {
      depth_result.blocker = std::string("Depth generation error: ") + e.what();
      depth_result.onnx_runtime_available = model_runtime_available;
    }

    summary.depth_index_written = depth_result.depth_index_written;
    summary.depth_blocks_written = depth_result.depth_blocks_written;
    summary.depth_generation_run = depth_result.depth_generation_run;
    summary.depth_model_available = depth_result.depth_model_available;
    summary.depth_generation_detail =
        svp::vision::depth_generation_result_to_json(depth_result);

    svp::vision::EmbeddingGenerationOptions emb_opts;
    emb_opts.model_cache_root = model_cache_root;

    svp::vision::EmbeddingGenerationResult emb_result;
    try {
      emb_result = svp::vision::generate_embedding_blocks(
          emb_opts, staging_dir);
    } catch (const std::exception& e) {
      emb_result.blocker = std::string("Embedding generation error: ") + e.what();
      emb_result.onnx_runtime_available = model_runtime_available;
    }

    summary.embedding_sets_written = emb_result.embedding_sets_written;
    summary.embeddings_index_written = emb_result.embeddings_index_written;
    summary.embeddings_blocks_written = emb_result.embeddings_blocks_written;
    summary.embedding_generation_run = emb_result.embedding_generation_run;
    summary.embedding_model_available = emb_result.embedding_model_available;
    summary.embedding_generation_detail =
        svp::vision::embedding_generation_result_to_json(emb_result);

    if (!depth_result.depth_blocks_written) {
      write_empty_file(staging_dir / "spatial" / "depth.index.jsonl");
      summary.depth_index_written = true;
      write_empty_file(staging_dir / "spatial" / "depth.blocks.svpdz");
      summary.depth_blocks_written = true;
    }

    write_empty_file(staging_dir / "spatial" / "masks.index.jsonl");
    summary.masks_index_written = true;
    write_empty_file(staging_dir / "spatial" / "masks.blocks.svpmz");
    summary.masks_blocks_written = true;

    if (!emb_result.embeddings_blocks_written) {
      write_text_file(staging_dir / "embeddings" / "embedding_sets.json",
                      "{\"sets\": []}\n");
      summary.embedding_sets_written = true;
      write_empty_file(staging_dir / "embeddings" / "embeddings.index.jsonl");
      summary.embeddings_index_written = true;
      write_empty_file(staging_dir / "embeddings" / "embeddings.blocks.svpez");
      summary.embeddings_blocks_written = true;
    }

    std::vector<nlohmann::json> processors;
    if (depth_result.processor_provenance.is_object()) {
      processors.push_back(depth_result.processor_provenance);
    } else {
      processors.push_back(make_spatial_placeholder_processor());
    }
    if (emb_result.processor_provenance.is_object()) {
      processors.push_back(emb_result.processor_provenance);
    } else {
      processors.push_back(make_embedding_placeholder_processor());
    }
    append_processor_records(staging_dir / "provenance" / "processors.jsonl",
                             processors);
    summary.provenance_records_added = processors.size();

    return summary;
  }

  summary.depth_generation_run = false;
  summary.embedding_generation_run = false;

  write_empty_file(staging_dir / "spatial" / "depth.index.jsonl");
  summary.depth_index_written = true;

  write_empty_file(staging_dir / "spatial" / "depth.blocks.svpdz");
  summary.depth_blocks_written = true;

  write_empty_file(staging_dir / "spatial" / "masks.index.jsonl");
  summary.masks_index_written = true;

  write_empty_file(staging_dir / "spatial" / "masks.blocks.svpmz");
  summary.masks_blocks_written = true;

  write_text_file(staging_dir / "embeddings" / "embedding_sets.json",
                  "{\"sets\": []}\n");
  summary.embedding_sets_written = true;

  write_empty_file(staging_dir / "embeddings" / "embeddings.index.jsonl");
  summary.embeddings_index_written = true;

  write_empty_file(staging_dir / "embeddings" / "embeddings.blocks.svpez");
  summary.embeddings_blocks_written = true;

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
      {"depth_blocks_written", summary.depth_blocks_written != 0},
      {"depth_generation_run", summary.depth_generation_run != 0},
      {"depth_model_available", summary.depth_model_available != 0},
      {"masks_index_written", summary.masks_index_written != 0},
      {"masks_blocks_written", summary.masks_blocks_written != 0},
      {"embedding_sets_written", summary.embedding_sets_written != 0},
      {"embeddings_index_written", summary.embeddings_index_written != 0},
      {"embeddings_blocks_written", summary.embeddings_blocks_written != 0},
      {"embedding_generation_run", summary.embedding_generation_run != 0},
      {"embedding_model_available", summary.embedding_model_available != 0},
      {"model_runtime_available", summary.model_runtime_available != 0},
      {"provenance_records_added", summary.provenance_records_added},
      {"depth_generation_detail", summary.depth_generation_detail},
      {"embedding_generation_detail", summary.embedding_generation_detail},
  };
}

}  // namespace svp::package
