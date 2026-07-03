#include "ocr_generation_internal.hpp"

#include "svp/media/media_ingest_plan.hpp"
#include "svp/vision/evidence_crop.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sys/wait.h>

namespace svp::vision::ocr_generation_internal {
namespace {

std::string shell_quote(const std::filesystem::path& path) {
  std::string quoted = "'";
  for (const char c : path.string()) {
    if (c == '\'') {
      quoted += "'\\''";
    } else {
      quoted += c;
    }
  }
  quoted += "'";
  return quoted;
}

std::optional<ColorRasterFrame> decode_crop_image_with_ffmpeg(
    const std::filesystem::path& ffmpeg_path,
    const std::filesystem::path& image_path,
    int width,
    int height,
    const std::string& frame_id,
    std::int64_t timestamp_us) {
  if (width <= 0 || height <= 0 || !std::filesystem::exists(image_path)) {
    return std::nullopt;
  }

  const std::string cmd =
      shell_quote(ffmpeg_path) +
      " -v error"
      " -i " + shell_quote(image_path) +
      " -vf scale=" + std::to_string(width) + ":" + std::to_string(height) +
      " -vframes 1"
      " -f rawvideo"
      " -pix_fmt rgb24"
      " pipe:1"
      " 2>/dev/null";

  FILE* pipe = popen(cmd.c_str(), "r");
  if (pipe == nullptr) return std::nullopt;

  const std::size_t expected_bytes =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3;
  std::vector<std::uint8_t> raw_bytes(expected_bytes);
  const std::size_t bytes_read = std::fread(raw_bytes.data(), 1, expected_bytes, pipe);
  const int status = pclose(pipe);

  if (bytes_read != expected_bytes ||
      !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return std::nullopt;
  }

  ColorRasterFrame frame;
  frame.frame_id = frame_id;
  frame.frame_index = 0;
  frame.timestamp_us = timestamp_us;
  frame.width = width;
  frame.height = height;
  frame.keyframe = false;
  frame.pixels.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
  for (std::size_t i = 0; i < raw_bytes.size(); i += 3) {
    frame.pixels.push_back({raw_bytes[i], raw_bytes[i + 1], raw_bytes[i + 2]});
  }
  return frame;
}

std::vector<CropGenerationInput> build_crop_inputs(
    const OcrGenerationResult& result,
    const std::vector<ReconciledObservation>& reconciled) {
  std::vector<CropGenerationInput> crop_inputs;
  crop_inputs.reserve(result.text_observations.size());

  for (std::size_t i = 0; i < result.text_observations.size(); ++i) {
    const auto& obs = result.text_observations[i];
    const auto& robs = reconciled[i];

    CropGenerationInput input;
    input.text_region_id = obs.text_region_id;
    input.text_observation_id = obs.text_observation_id;
    if (!obs.source_frame_ids.empty()) {
      input.source_frame_id = obs.source_frame_ids[0];
    }
    input.source_timestamp_us = robs.start_us;
    input.bbox_left = robs.bbox_left;
    input.bbox_top = robs.bbox_top;
    input.bbox_right = robs.bbox_right;
    input.bbox_bottom = robs.bbox_bottom;
    input.frame_width = robs.frame_width;
    input.frame_height = robs.frame_height;
    input.confidence = robs.confidence;
    input.detection_count = robs.detection_count;
    input.observation_raw_text = obs.raw_text;
    crop_inputs.push_back(std::move(input));
  }

  return crop_inputs;
}

EvidenceCropOptions build_crop_options(const OcrGenerationOptions& options) {
  const OcrSourceFrameDimensions source_dims =
      derive_ocr_source_frame_dimensions(
          static_cast<int>(options.media_plan->primary_video_stream.width),
          static_cast<int>(options.media_plan->primary_video_stream.height),
          static_cast<int>(options.media_plan->primary_video_stream.rotation_degrees));

  EvidenceCropOptions crop_opts;
  crop_opts.ffmpeg_path = options.ffmpeg_path;
  crop_opts.source_media_path = options.media_plan->source_path;
  crop_opts.ocr_frame_width = options.ocr_frame_width;
  crop_opts.ocr_frame_height = options.ocr_frame_height;
  crop_opts.source_frame_width = source_dims.width;
  crop_opts.source_frame_height = source_dims.height;
  crop_opts.canonical_raster_width = options.canonical_raster_width;
  crop_opts.canonical_raster_height = options.canonical_raster_height;
  crop_opts.max_total_crops = options.max_total_crops;
  crop_opts.max_total_crop_bytes = options.max_total_crop_bytes;
  crop_opts.crop_coverage_policy = options.crop_coverage_policy;
  crop_opts.min_jpeg_quality = options.crop_min_jpeg_quality;
  crop_opts.crop_image_format = "jpeg";
  crop_opts.jpeg_quality = 95;
  return crop_opts;
}

void copy_crop_result_to_generation_result(
    const EvidenceCropResult& crop_result,
    OcrGenerationResult& result) {
  result.evidence_crops = crop_result.crops;
  result.evidence_crop_count = crop_result.crop_count;
  result.evidence_crop_total_bytes = crop_result.total_crop_bytes;
  result.evidence_crops_written = crop_result.crops_written;
  result.evidence_crops_skipped = crop_result.crops_skipped_count;
  result.evidence_crops_skipped_reason = crop_result.crops_skipped_reason;
  result.crop_coverage_policy = crop_result.crop_coverage_policy;
  result.crop_effective_max_total_crops = crop_result.effective_max_total_crops;
  result.crop_effective_max_total_crop_bytes = crop_result.effective_max_total_crop_bytes;
  result.crops_skipped_by_count_cap = crop_result.crops_skipped_by_count_cap;
  result.crops_skipped_by_byte_cap = crop_result.crops_skipped_by_byte_cap;
  result.crops_skipped_by_extraction = crop_result.crops_skipped_by_extraction;
  result.crop_total_observations_requested = crop_result.total_observations_requested;
  result.every_observation_has_crop = crop_result.every_observation_has_crop;
  result.crop_coverage_status = crop_result.crop_coverage_status;
}

void rewrite_evidence_crop_jsonl(const std::filesystem::path& staging_dir,
                                 const OcrGenerationResult& result) {
  std::ofstream out(staging_dir / "text" / "evidence_crops.jsonl");
  if (!out) return;
  for (const auto& crop : result.evidence_crops) {
    out << evidence_crop_to_json(crop).dump() << "\n";
  }
}

}  // namespace

RoiHardeningSummary generate_and_harden_evidence_crops(
    const OcrGenerationOptions& options,
    const std::vector<ReconciledObservation>& reconciled,
    const PpOcrSession& pp_ocr_session,
    const PpOcrOptions& pp_ocr_opts,
    const std::filesystem::path& staging_dir,
    OcrGenerationResult& result) {
  RoiHardeningSummary summary;
  if (!options.generate_evidence_crops ||
      options.media_plan == nullptr ||
      result.text_observations.empty()) {
    return summary;
  }

  EvidenceCropResult crop_result;
  try {
    crop_result = generate_evidence_crops_internal(
        build_crop_options(options),
        build_crop_inputs(result, reconciled),
        staging_dir);
  } catch (const std::exception& e) {
    crop_result.crops_written = false;
    crop_result.crops_skipped_reason =
        std::string("Evidence crop generation error: ") + e.what();
  }

  copy_crop_result_to_generation_result(crop_result, result);
  result.roi_hardening_run = true;

  std::map<std::string, std::size_t> observation_index_by_id;
  for (std::size_t i = 0; i < result.text_observations.size(); ++i) {
    observation_index_by_id[result.text_observations[i].text_observation_id] = i;
  }

  for (auto& crop : result.evidence_crops) {
    auto obs_it = observation_index_by_id.find(crop.text_observation_id);
    if (obs_it == observation_index_by_id.end()) {
      crop.evidence_quality = "weak";
      crop.evidence_quality_reason = "Linked text observation was not found";
      continue;
    }

    auto& obs = result.text_observations[obs_it->second];
    if (std::find(obs.evidence_crop_refs.begin(),
                  obs.evidence_crop_refs.end(),
                  crop.crop_id) == obs.evidence_crop_refs.end()) {
      obs.evidence_crop_refs.push_back(crop.crop_id);
    }

    const int crop_width = crop.crop_bbox_right - crop.crop_bbox_left;
    const int crop_height = crop.crop_bbox_bottom - crop.crop_bbox_top;
    std::optional<ColorRasterFrame> crop_frame =
        decode_crop_image_with_ffmpeg(
            options.ffmpeg_path,
            staging_dir / crop.crop_file_path,
            crop_width,
            crop_height,
            crop.source_frame_id,
            crop.source_timestamp_us);
    if (!crop_frame.has_value()) {
      crop.evidence_quality = "unsupported";
      crop.evidence_quality_reason =
          "Crop image could not be decoded for ROI OCR verification";
      continue;
    }

    const PpOcrDetection roi_detection =
        run_pp_ocr_recognition_on_crop(pp_ocr_session, pp_ocr_opts, *crop_frame);
    if (roi_detection.text.empty()) {
      crop.evidence_quality = "weak";
      crop.evidence_quality_reason = "Crop was decoded but ROI OCR produced no text";
      continue;
    }

    crop.roi_ocr_text = roi_detection.text;
    crop.roi_ocr_confidence = roi_detection.score;
    crop.roi_ocr_word_count =
        alphanumeric_key(normalize_text(roi_detection.text)).empty() ? 0 : 1;

    if (roi_text_is_better(obs.raw_text, roi_detection.text)) {
      obs.raw_text = roi_detection.text;
      obs.normalized_text = normalize_text(roi_detection.text);
      obs.confidence = std::max(obs.confidence, roi_detection.score);
      if (obs.language.has_value()) {
        (*obs.language)["confidence"] = obs.confidence;
      }
      ++summary.improved_observation_count;
    }

    crop.evidence_quality = "strong";
    crop.evidence_quality_reason =
        "Crop was decoded and PP-OCR ROI re-read produced text for the linked observation";
    ++summary.verified_crop_count;
  }

  rewrite_evidence_crop_jsonl(staging_dir, result);
  return summary;
}

}  // namespace svp::vision::ocr_generation_internal
