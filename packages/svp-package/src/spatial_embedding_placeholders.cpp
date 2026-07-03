#include "svp/package/spatial_embedding_placeholders.hpp"

#include "svp/media/media_ingest_plan.hpp"
#include "svp/vision/canonical_frame_input.hpp"
#include "svp/vision/depth_generation.hpp"
#include "svp/vision/embedding_generation.hpp"
#include "svp/vision/frame_catalog.hpp"
#include "svp/vision/ocr_generation.hpp"
#include "svp/vision/visual_entity_tracker.hpp"
#include "svp/package/entity_writer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
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

void attach_ocr_progress_callbacks(svp::vision::OcrGenerationOptions& ocr_opts,
                                   const SpatialProgressCallback& on_progress) {
  if (!on_progress) {
    return;
  }

  auto evidence_observation_total = std::make_shared<std::size_t>(0);
  ocr_opts.on_progress = [&on_progress](int current, int total) {
    on_progress("ocr",
                static_cast<std::size_t>(current),
                static_cast<std::size_t>(total),
                "");
  };
  ocr_opts.on_evidence_crop_progress =
      [on_progress, evidence_observation_total](std::size_t current,
                                                std::size_t total) {
        if (total == 0) {
          return;
        }
        *evidence_observation_total = total;
        on_progress("ocr_evidence_crops",
                    current,
                    *evidence_observation_total * 2,
                    "extracting evidence crops");
      };
  ocr_opts.on_evidence_roi_progress =
      [on_progress, evidence_observation_total](std::size_t current,
                                                std::size_t /*total*/) {
        if (*evidence_observation_total == 0) {
          return;
        }
        on_progress("ocr_evidence_crops",
                    *evidence_observation_total + current,
                    *evidence_observation_total * 2,
                    "verifying evidence crops");
      };
}

}  // namespace

SpatialEmbeddingPlaceholderSummary write_spatial_and_embedding_placeholders(
    const std::filesystem::path& staging_dir,
    bool model_runtime_available,
    const nlohmann::json& media_plan_json,
    const std::filesystem::path& model_cache_root,
    const svp::media::MediaIngestPlan* media_plan,
    const std::filesystem::path& ffmpeg_path,
    svp::vision::FrameCatalog* frame_catalog,
    SpatialProgressCallback on_progress) {
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

    // Decode real canonical frames from source media once and share across
    // depth generation and OCR generation.
    svp::vision::DecodedCanonicalFrames decoded_frames;
    if (media_plan != nullptr && !ffmpeg_path.empty()) {
      decoded_frames = svp::vision::decode_canonical_frames(*media_plan, ffmpeg_path,
                                                             frame_catalog);
    }

    // Run OCR generation on decoded frames before embedding generation so
    // that text_observations.jsonl is populated when embedding generation
    // reads it.  OCR uses PP-OCR ONNX models (native, no Python).
    // OCR decodes its own higher-resolution frames for text detection.
    svp::vision::OcrGenerationOptions ocr_opts;
    ocr_opts.model_cache_root = model_cache_root;
    ocr_opts.ffmpeg_path = ffmpeg_path;
    ocr_opts.media_plan = media_plan;
    ocr_opts.canonical_raster_width = static_cast<int>(raster_w);
    ocr_opts.canonical_raster_height = static_cast<int>(raster_h);
    // Decode OCR frames at a higher resolution for text detection.
    // Use the source display dimensions capped at 1920x1080 to preserve
    // text readability while keeping processing reasonable.
    if (media_plan != nullptr) {
      int src_w = static_cast<int>(media_plan->primary_video_stream.width);
      int src_h = static_cast<int>(media_plan->primary_video_stream.height);
      // Account for rotation: if rotation is 90 or 270, swap dimensions
      if (std::abs(media_plan->primary_video_stream.rotation_degrees) == 90) {
        std::swap(src_w, src_h);
      }
      const int max_ocr_dim = 1920;
      if (src_w > max_ocr_dim || src_h > max_ocr_dim) {
        if (src_w >= src_h) {
          ocr_opts.ocr_frame_width = max_ocr_dim;
          ocr_opts.ocr_frame_height = static_cast<int>(
              std::round(static_cast<double>(src_h) * max_ocr_dim / src_w));
        } else {
          ocr_opts.ocr_frame_height = max_ocr_dim;
          ocr_opts.ocr_frame_width = static_cast<int>(
              std::round(static_cast<double>(src_w) * max_ocr_dim / src_h));
        }
      } else {
        ocr_opts.ocr_frame_width = src_w;
        ocr_opts.ocr_frame_height = src_h;
      }
    }

    // Enable evidence crop generation for text regions.
    // This extracts bounded crop images as visual evidence.
    ocr_opts.generate_evidence_crops = (media_plan != nullptr);
    ocr_opts.crop_coverage_policy = "one_per_observation";
    ocr_opts.crop_min_jpeg_quality = 50;
    ocr_opts.frame_catalog = frame_catalog;
    attach_ocr_progress_callbacks(ocr_opts, on_progress);

    svp::vision::OcrGenerationResult ocr_result;
    try {
      ocr_result = svp::vision::generate_ocr_observations(
          ocr_opts, decoded_frames, staging_dir);
    } catch (const std::exception& e) {
      ocr_result.blocker = std::string("OCR generation error: ") + e.what();
    }

    summary.ocr_available = ocr_result.ocr_available;
    summary.ocr_frame_input_available = ocr_result.ocr_frame_input_available;
    summary.ocr_detection_run = ocr_result.ocr_detection_run;
    summary.ocr_recognition_run = ocr_result.ocr_recognition_run;
    summary.text_observation_count =
        static_cast<std::size_t>(ocr_result.text_observation_count);
    summary.numeric_value_count =
        static_cast<std::size_t>(ocr_result.numeric_value_count);
    summary.ocr_generation_detail =
        svp::vision::ocr_generation_result_to_json(ocr_result);

    // Append OCR processor provenance records
    if (!ocr_result.processors.empty()) {
      append_processor_records(staging_dir / "provenance" / "processors.jsonl",
                               ocr_result.processors);
    }

    svp::vision::DepthGenerationOptions depth_opts;
    depth_opts.model_cache_root = model_cache_root;
    depth_opts.raster_width = raster_w;
    depth_opts.raster_height = raster_h;
    depth_opts.frame_input = decoded_frames;
    if (on_progress) {
      depth_opts.on_progress = [&on_progress](std::size_t current, std::size_t total) {
        on_progress("depth", current, total, "");
      };
    }
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
    summary.depth_model_verified = depth_result.depth_model_verified;
    // Report frame input availability from the decoded frames directly,
    // not from depth_result which may have returned early at model gating.
    summary.depth_frame_input_available =
        decoded_frames.decoding_succeeded &&
        !decoded_frames.frames.empty();
    summary.depth_generation_detail =
        svp::vision::depth_generation_result_to_json(depth_result);

    svp::vision::EmbeddingGenerationOptions emb_opts;
    emb_opts.model_cache_root = model_cache_root;
    if (on_progress) {
      emb_opts.on_progress = [&on_progress](std::size_t current, std::size_t total) {
        on_progress("text_embeddings", current, total, "");
      };
    }

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
      summary.depth_placeholder_written = true;
      // Do NOT set depth_blocks_written — that field means real SVPB depth
      // blocks were generated. The placeholder is an honest empty file.
    }

    // Run visual entity tracker on decoded frames with depth data
    // per spec §20.6. This produces spatial regions, masks, entity tracks,
    // and visual embeddings.
    if (decoded_frames.decoding_succeeded && !decoded_frames.frames.empty()) {
      svp::vision::VisualEntityTrackerOptions tracker_opts;
      tracker_opts.embedding_model_id = "model_nomic_embed_vision_v1_5";
      tracker_opts.execution_provider = "cpu";
      if (on_progress) {
        tracker_opts.on_tracking_progress = [&on_progress](std::size_t current, std::size_t total) {
          on_progress("visual_tracking", current, total, "");
        };
        tracker_opts.on_visual_embedding_progress = [&on_progress](std::size_t current, std::size_t total) {
          on_progress("visual_embeddings", current, total, "");
        };
      }

      // Read shot boundaries from timeline
      std::vector<std::pair<std::string, std::int64_t>> shot_boundaries;
      auto shots = read_jsonl(staging_dir / "timeline" / "shots.jsonl");
      for (const auto& shot : shots) {
        if (shot.contains("id") && shot.contains("start_us")) {
          shot_boundaries.emplace_back(
              shot["id"].get<std::string>(),
              shot["start_us"].get<std::int64_t>());
        }
      }

      // Read depth frame IDs from depth index
      std::vector<std::string> depth_frame_ids;
      auto depth_index = read_jsonl(staging_dir / "spatial" / "depth.index.jsonl");
      for (const auto& entry : depth_index) {
        if (entry.contains("frame_id")) {
          depth_frame_ids.push_back(entry["frame_id"].get<std::string>());
        }
      }

      auto tracker_result = svp::vision::run_visual_entity_tracker(
          decoded_frames.frames,
          depth_result.raw_depth_data,
          depth_frame_ids,
          shot_boundaries,
          model_cache_root,
          tracker_opts);

      // Write visual entity artifacts (entities, tracks, regions, masks)
      auto visual_entity_summary = svp::package::write_visual_entity_artifacts(
          staging_dir, tracker_result);
      summary.masks_index_written = visual_entity_summary.masks_written;
      summary.masks_blocks_written = visual_entity_summary.masks_written;

      depth_result.raw_depth_data.clear();
      depth_result.raw_depth_data.shrink_to_fit();
      decoded_frames.frames.clear();
      decoded_frames.frames.shrink_to_fit();
    }

    if (!summary.masks_index_written) {
      write_empty_file(staging_dir / "spatial" / "masks.index.jsonl");
      summary.masks_index_written = true;
      write_empty_file(staging_dir / "spatial" / "masks.blocks.svpmz");
      summary.masks_blocks_written = true;
    }

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
    summary.provenance_records_added += processors.size();

    return summary;
  }

  summary.depth_generation_run = false;
  summary.embedding_generation_run = false;

  // Even without ONNX Runtime, PP-OCR can run if model bundles are available.
  // Decode frames and run OCR so text artifacts are produced.
  svp::vision::DecodedCanonicalFrames decoded_frames;
  if (media_plan != nullptr && !ffmpeg_path.empty()) {
    decoded_frames = svp::vision::decode_canonical_frames(*media_plan, ffmpeg_path,
                                                           frame_catalog);
  }

  // Determine canonical raster dimensions for bbox normalization
  std::uint32_t raster_w = 640;
  std::uint32_t raster_h = 360;
  if (!media_plan_json.empty() && media_plan_json.contains("canonical_raster")) {
    const auto& raster = media_plan_json["canonical_raster"];
    if (raster.contains("width") && raster["width"].is_number())
      raster_w = raster["width"].get<std::uint32_t>();
    if (raster.contains("height") && raster["height"].is_number())
      raster_h = raster["height"].get<std::uint32_t>();
  }

  svp::vision::OcrGenerationOptions ocr_opts;
  ocr_opts.model_cache_root = model_cache_root;
  ocr_opts.ffmpeg_path = ffmpeg_path;
  ocr_opts.media_plan = media_plan;
  ocr_opts.canonical_raster_width = static_cast<int>(raster_w);
  ocr_opts.canonical_raster_height = static_cast<int>(raster_h);
  if (media_plan != nullptr) {
    int src_w = static_cast<int>(media_plan->primary_video_stream.width);
    int src_h = static_cast<int>(media_plan->primary_video_stream.height);
    if (std::abs(media_plan->primary_video_stream.rotation_degrees) == 90) {
      std::swap(src_w, src_h);
    }
    const int max_ocr_dim = 1920;
    if (src_w > max_ocr_dim || src_h > max_ocr_dim) {
      if (src_w >= src_h) {
        ocr_opts.ocr_frame_width = max_ocr_dim;
        ocr_opts.ocr_frame_height = static_cast<int>(
            std::round(static_cast<double>(src_h) * max_ocr_dim / src_w));
      } else {
        ocr_opts.ocr_frame_height = max_ocr_dim;
        ocr_opts.ocr_frame_width = static_cast<int>(
            std::round(static_cast<double>(src_w) * max_ocr_dim / src_h));
      }
    } else {
      ocr_opts.ocr_frame_width = src_w;
      ocr_opts.ocr_frame_height = src_h;
    }
  }

  ocr_opts.frame_catalog = frame_catalog;
  attach_ocr_progress_callbacks(ocr_opts, on_progress);

  svp::vision::OcrGenerationResult ocr_result;
  try {
    ocr_result = svp::vision::generate_ocr_observations(
        ocr_opts, decoded_frames, staging_dir);
  } catch (const std::exception& e) {
    ocr_result.blocker = std::string("OCR generation error: ") + e.what();
  }

  summary.ocr_available = ocr_result.ocr_available;
  summary.ocr_frame_input_available = ocr_result.ocr_frame_input_available;
  summary.ocr_detection_run = ocr_result.ocr_detection_run;
  summary.ocr_recognition_run = ocr_result.ocr_recognition_run;
  summary.text_observation_count =
      static_cast<std::size_t>(ocr_result.text_observation_count);
  summary.numeric_value_count =
      static_cast<std::size_t>(ocr_result.numeric_value_count);
  summary.ocr_generation_detail =
      svp::vision::ocr_generation_result_to_json(ocr_result);

  if (!ocr_result.processors.empty()) {
    append_processor_records(staging_dir / "provenance" / "processors.jsonl",
                             ocr_result.processors);
  }

  write_empty_file(staging_dir / "spatial" / "depth.index.jsonl");
  summary.depth_index_written = true;

  write_empty_file(staging_dir / "spatial" / "depth.blocks.svpdz");
  summary.depth_placeholder_written = true;

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
  summary.provenance_records_added += placeholder_processors.size();

  return summary;
}

nlohmann::json spatial_embedding_placeholder_summary_to_json(
    const SpatialEmbeddingPlaceholderSummary& summary) {
  return {
      {"depth_index_written", summary.depth_index_written != 0},
      {"depth_blocks_written", summary.depth_blocks_written != 0},
      {"depth_placeholder_written", summary.depth_placeholder_written != 0},
      {"depth_generation_run", summary.depth_generation_run != 0},
      {"depth_model_available", summary.depth_model_available != 0},
      {"depth_model_verified", summary.depth_model_verified != 0},
      {"depth_frame_input_available", summary.depth_frame_input_available != 0},
      {"masks_index_written", summary.masks_index_written != 0},
      {"masks_blocks_written", summary.masks_blocks_written != 0},
      {"embedding_sets_written", summary.embedding_sets_written != 0},
      {"embeddings_index_written", summary.embeddings_index_written != 0},
      {"embeddings_blocks_written", summary.embeddings_blocks_written != 0},
      {"embedding_generation_run", summary.embedding_generation_run != 0},
      {"embedding_model_available", summary.embedding_model_available != 0},
      {"model_runtime_available", summary.model_runtime_available != 0},
      {"provenance_records_added", summary.provenance_records_added},
      {"ocr_available", summary.ocr_available != 0},
      {"ocr_frame_input_available", summary.ocr_frame_input_available != 0},
      {"ocr_detection_run", summary.ocr_detection_run != 0},
      {"ocr_recognition_run", summary.ocr_recognition_run != 0},
      {"text_observation_count", summary.text_observation_count},
      {"numeric_value_count", summary.numeric_value_count},
      {"depth_generation_detail", summary.depth_generation_detail},
      {"embedding_generation_detail", summary.embedding_generation_detail},
      {"ocr_generation_detail", summary.ocr_generation_detail},
  };
}

}  // namespace svp::package
