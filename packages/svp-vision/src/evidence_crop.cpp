#include "svp/vision/evidence_crop.hpp"

#include "svp/models/hash.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <vector>

namespace svp::vision {
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

std::string shell_quote_str(const std::string& s) {
  std::string quoted = "'";
  for (const char c : s) {
    if (c == '\'') {
      quoted += "'\\''";
    } else {
      quoted += c;
    }
  }
  quoted += "'";
  return quoted;
}

std::string trim(const std::string& s) {
  std::size_t start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  std::size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

std::string microseconds_to_seek_string(std::int64_t us) {
  const std::int64_t seconds = us / 1000000;
  const std::int64_t fraction = us % 1000000;
  std::ostringstream oss;
  oss << seconds << "." << std::setw(6) << std::setfill('0') << fraction;
  return oss.str();
}

std::string pad_id(const std::string& prefix, int index, int width = 6) {
  std::ostringstream oss;
  oss << prefix << std::setw(width) << std::setfill('0') << index;
  return oss.str();
}

// Extract a crop from the source video at a given timestamp and bbox.
bool extract_crop_from_source(
    const std::filesystem::path& ffmpeg_path,
    const std::filesystem::path& source_path,
    std::int64_t seek_us,
    int crop_left, int crop_top, int crop_width, int crop_height,
    int target_width, int target_height,
    const std::string& image_format,
    int jpeg_quality,
    const std::filesystem::path& output_path,
    std::string& error) {
  const std::string seek = microseconds_to_seek_string(seek_us);

  std::string crop_filter =
      "crop=" + std::to_string(crop_width) + ":" +
      std::to_string(crop_height) + ":" +
      std::to_string(crop_left) + ":" +
      std::to_string(crop_top);

  const int min_ocr_width = 500;
  const int max_crop_width = 2000;
  if (target_width < min_ocr_width) {
    const int scaled_width = std::min(target_width * 2, max_crop_width);
    const int scaled_height = static_cast<int>(
        std::round(static_cast<double>(target_height) * scaled_width /
                   std::max(1, target_width)));
    crop_filter += ",scale=" + std::to_string(scaled_width) + ":" +
                    std::to_string(scaled_height);
  } else if (target_width > max_crop_width) {
    const int scaled_width = max_crop_width;
    const int scaled_height = static_cast<int>(
        std::round(static_cast<double>(target_height) * scaled_width /
                   std::max(1, target_width)));
    crop_filter += ",scale=" + std::to_string(scaled_width) + ":" +
                    std::to_string(scaled_height);
  }

  std::string codec_opts;
  if (image_format == "jpeg" || image_format == "jpg") {
    codec_opts = " -c:v mjpeg -q:v " + std::to_string(
        std::max(1, std::min(31, 31 - (jpeg_quality * 31) / 100)));
    crop_filter += ",format=yuvj420p";
  }

  std::string cmd =
      shell_quote(ffmpeg_path) +
      " -v error"
      " -ss " + seek +
      " -i " + shell_quote(source_path) +
      " -vf " + shell_quote_str(crop_filter) +
      " -vframes 1" +
      codec_opts +
      " -y " + shell_quote(output_path) +
      " 2>&1";

  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    error = "popen failed: " + std::string(std::strerror(errno));
    return false;
  }

  std::string output;
  char buffer[4096];
  while (true) {
    const std::size_t n = fread(buffer, 1, sizeof(buffer), pipe);
    if (n == 0) break;
    output.append(buffer, n);
  }
  const int status = pclose(pipe);
  const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
      std::filesystem::exists(output_path);

  if (!ok) {
    error = trim(output);
    if (error.empty()) {
      error = "ffmpeg exited with status " +
              std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : status);
    }
  }
  return ok;
}

void expand_text_line_bbox(
    int bbox_left, int bbox_top, int bbox_right, int bbox_bottom,
    int frame_width, int frame_height,
    int& crop_left, int& crop_top, int& crop_width, int& crop_height) {
  const int bw = std::max(1, bbox_right - bbox_left);
  const int bh = std::max(1, bbox_bottom - bbox_top);
  const int target_width = std::min(
      frame_width,
      std::max(
          bw + static_cast<int>(std::round(static_cast<double>(bh) * 4.0)),
          static_cast<int>(std::round(static_cast<double>(bh) * 14.0))));
  const int center_x = bbox_left + bw / 2;
  const int margin_y = static_cast<int>(
      std::round(static_cast<double>(bh) * 0.55));

  crop_left = center_x - target_width / 2;
  crop_left = std::max(0, std::min(crop_left, frame_width - target_width));
  crop_top = std::max(0, bbox_top - margin_y);
  int crop_right = crop_left + target_width;
  int crop_bottom = std::min(frame_height, bbox_bottom + margin_y);

  crop_width = crop_right - crop_left;
  crop_height = crop_bottom - crop_top;

  if (crop_width < 1) crop_width = 1;
  if (crop_height < 1) crop_height = 1;
}

std::int64_t get_file_size(const std::filesystem::path& path) {
  std::error_code ec;
  auto size = std::filesystem::file_size(path, ec);
  if (ec) return 0;
  return static_cast<std::int64_t>(size);
}

}  // namespace

nlohmann::json evidence_crop_to_json(const EvidenceCropRecord& record) {
  nlohmann::json j = {
      {"crop_id", record.crop_id},
      {"text_region_id", record.text_region_id},
      {"text_observation_id", record.text_observation_id},
      {"source_frame_id", record.source_frame_id},
      {"source_timestamp_us", record.source_timestamp_us},
      {"original_bbox", {
          record.original_bbox_left,
          record.original_bbox_top,
          record.original_bbox_right,
          record.original_bbox_bottom
      }},
      {"crop_bbox_ocr", {
          record.crop_bbox_ocr_left,
          record.crop_bbox_ocr_top,
          record.crop_bbox_ocr_right,
          record.crop_bbox_ocr_bottom
      }},
      {"crop_bbox", {
          record.crop_bbox_left,
          record.crop_bbox_top,
          record.crop_bbox_right,
          record.crop_bbox_bottom
      }},
      {"ocr_frame_width", record.ocr_frame_width},
      {"ocr_frame_height", record.ocr_frame_height},
      {"source_frame_width", record.source_frame_width},
      {"source_frame_height", record.source_frame_height},
      {"canonical_raster_width", record.canonical_raster_width},
      {"canonical_raster_height", record.canonical_raster_height},
      {"bbox_coordinate_space", record.bbox_coordinate_space},
      {"transform_scale_x", record.transform_scale_x},
      {"transform_scale_y", record.transform_scale_y},
      {"crop_extraction_method", record.crop_extraction_method},
      {"frame_width", record.frame_width},
      {"frame_height", record.frame_height},
      {"crop_transform", record.crop_transform},
      {"image_format", record.image_format},
      {"crop_file_path", record.crop_file_path},
      {"crop_size_bytes", record.crop_size_bytes},
      {"blake3_hash", record.blake3_hash},
      {"selection_reason", record.selection_reason},
      {"evidence_quality", record.evidence_quality},
      {"evidence_quality_reason", record.evidence_quality_reason},
      {"roi_ocr_text", record.roi_ocr_text},
      {"roi_ocr_confidence", record.roi_ocr_confidence},
      {"roi_ocr_word_count", record.roi_ocr_word_count},
  };
  return j;
}

nlohmann::json evidence_crop_result_to_json(const EvidenceCropResult& result) {
  nlohmann::json crops_arr = nlohmann::json::array();
  for (const auto& crop : result.crops) {
    crops_arr.push_back(evidence_crop_to_json(crop));
  }
  return {
      {"crops_written", result.crops_written},
      {"crop_count", result.crop_count},
      {"total_crop_bytes", result.total_crop_bytes},
      {"crops_skipped_count", result.crops_skipped_count},
      {"crops_skipped_reason", result.crops_skipped_reason},
      {"crop_coverage_policy", result.crop_coverage_policy},
      {"effective_max_total_crops", result.effective_max_total_crops},
      {"effective_max_total_crop_bytes", result.effective_max_total_crop_bytes},
      {"crops_skipped_by_count_cap", result.crops_skipped_by_count_cap},
      {"crops_skipped_by_byte_cap", result.crops_skipped_by_byte_cap},
      {"crops_skipped_by_extraction", result.crops_skipped_by_extraction},
      {"total_observations_requested", result.total_observations_requested},
      {"every_observation_has_crop", result.every_observation_has_crop},
      {"crop_coverage_status", result.crop_coverage_status},
      {"crops", crops_arr},
  };
}

EvidenceCropResult generate_evidence_crops_internal(
    const EvidenceCropOptions& options,
    const std::vector<CropGenerationInput>& inputs,
    const std::filesystem::path& staging_dir) {
  EvidenceCropResult result;
  result.crop_coverage_policy = options.crop_coverage_policy;
  result.total_observations_requested = static_cast<std::int64_t>(inputs.size());

  const std::filesystem::path crops_dir = staging_dir / "text" / "evidence_crops";
  std::filesystem::create_directories(crops_dir);

  const std::string ext =
      (options.crop_image_format == "png") ? "png" : "jpg";

  // Determine effective max_total_crops and byte budget based on coverage policy.
  std::size_t effective_max_crops = options.max_total_crops;
  std::int64_t effective_byte_budget = options.max_total_crop_bytes;
  if (options.crop_coverage_policy == "one_per_observation") {
    effective_max_crops = std::max(options.max_total_crops, inputs.size());
    effective_byte_budget = std::numeric_limits<std::int64_t>::max();
  }
  result.effective_max_total_crops = effective_max_crops;
  result.effective_max_total_crop_bytes = effective_byte_budget;

  std::int64_t total_bytes = 0;
  std::size_t total_crops = 0;
  std::size_t crops_skipped = 0;
  std::int64_t skipped_by_count = 0;
  std::int64_t skipped_by_bytes = 0;
  std::int64_t skipped_by_extraction = 0;

  result.roi_ocr_results.resize(inputs.size());

  const int ocr_w = options.ocr_frame_width > 0 ?
      options.ocr_frame_width : 0;
  const int ocr_h = options.ocr_frame_height > 0 ?
      options.ocr_frame_height : 0;
  const int src_w = options.source_frame_width > 0 ?
      options.source_frame_width : ocr_w;
  const int src_h = options.source_frame_height > 0 ?
      options.source_frame_height : ocr_h;

  const double scale_x = (ocr_w > 0) ?
      static_cast<double>(src_w) / static_cast<double>(ocr_w) : 1.0;
  const double scale_y = (ocr_h > 0) ?
      static_cast<double>(src_h) / static_cast<double>(ocr_h) : 1.0;

  // Adaptive JPEG quality: start at configured quality, reduce if byte
  // budget is tight to fit more crops.
  int current_jpeg_quality = options.jpeg_quality;

  if (options.on_progress) {
    options.on_progress(0, inputs.size());
  }
  auto report_input_processed = [&](std::size_t index) {
    if (options.on_progress) {
      options.on_progress(index + 1, inputs.size());
    }
  };

  for (std::size_t i = 0; i < inputs.size(); ++i) {
    const auto& input = inputs[i];

    if (total_crops >= effective_max_crops) {
      crops_skipped++;
      skipped_by_count++;
      result.roi_ocr_results[i].succeeded = false;
      report_input_processed(i);
      continue;
    }

    if (total_bytes >= effective_byte_budget) {
      crops_skipped++;
      skipped_by_bytes++;
      result.roi_ocr_results[i].succeeded = false;
      report_input_processed(i);
      continue;
    }

    const int clamp_w = (ocr_w > 0) ? ocr_w : input.frame_width;
    const int clamp_h = (ocr_h > 0) ? ocr_h : input.frame_height;

    int ocr_crop_left, ocr_crop_top, ocr_crop_width, ocr_crop_height;
    expand_text_line_bbox(
        input.bbox_left, input.bbox_top,
        input.bbox_right, input.bbox_bottom,
        clamp_w, clamp_h,
        ocr_crop_left, ocr_crop_top, ocr_crop_width, ocr_crop_height);

    int src_crop_left = static_cast<int>(std::round(ocr_crop_left * scale_x));
    int src_crop_top = static_cast<int>(std::round(ocr_crop_top * scale_y));
    int src_crop_width = static_cast<int>(std::round(ocr_crop_width * scale_x));
    int src_crop_height = static_cast<int>(std::round(ocr_crop_height * scale_y));

    src_crop_left = std::max(0, std::min(src_crop_left, src_w));
    src_crop_top = std::max(0, std::min(src_crop_top, src_h));
    src_crop_width = std::min(src_crop_width, src_w - src_crop_left);
    src_crop_height = std::min(src_crop_height, src_h - src_crop_top);

    if (src_crop_width < 1 || src_crop_height < 1) {
      crops_skipped++;
      skipped_by_extraction++;
      result.roi_ocr_results[i].succeeded = false;
      report_input_processed(i);
      continue;
    }

    const std::string crop_id = pad_id("crop_", static_cast<int>(total_crops + 1));
    const std::string crop_filename = crop_id + "." + ext;
    const std::filesystem::path crop_path = crops_dir / crop_filename;

    std::string extract_error;
    bool crop_ok = extract_crop_from_source(
        options.ffmpeg_path, options.source_media_path,
        input.source_timestamp_us,
        src_crop_left, src_crop_top, src_crop_width, src_crop_height,
        src_crop_width, src_crop_height,
        options.crop_image_format,
        current_jpeg_quality,
        crop_path, extract_error);

    if (!crop_ok) {
      crops_skipped++;
      skipped_by_extraction++;
      result.roi_ocr_results[i].succeeded = false;
      report_input_processed(i);
      continue;
    }

    const std::int64_t crop_bytes = get_file_size(crop_path);

    if (total_bytes + crop_bytes > effective_byte_budget) {
      // If we haven't tried reducing quality yet and there are more
      // observations to process, try re-extracting at lower quality.
      if (current_jpeg_quality > options.min_jpeg_quality &&
          i + 1 < inputs.size()) {
        std::error_code rm_ec;
        std::filesystem::remove(crop_path, rm_ec);
        const int reduced_quality = std::max(
            options.min_jpeg_quality,
            current_jpeg_quality - 15);
        if (reduced_quality < current_jpeg_quality) {
          current_jpeg_quality = reduced_quality;
          // Re-try this crop at lower quality
          crop_ok = extract_crop_from_source(
              options.ffmpeg_path, options.source_media_path,
              input.source_timestamp_us,
              src_crop_left, src_crop_top, src_crop_width, src_crop_height,
              src_crop_width, src_crop_height,
              options.crop_image_format,
              current_jpeg_quality,
              crop_path, extract_error);
          if (crop_ok) {
            const std::int64_t reduced_bytes = get_file_size(crop_path);
            if (total_bytes + reduced_bytes <= effective_byte_budget) {
              // Accept the reduced-quality crop
              total_bytes += reduced_bytes;

              std::string blake3_hash;
              try {
                blake3_hash = svp::models::blake3_hex_for_file(crop_path);
              } catch (...) {
                blake3_hash = "";
              }

              EvidenceCropRecord crop;
              crop.crop_id = crop_id;
              crop.text_region_id = input.text_region_id;
              crop.text_observation_id = input.text_observation_id;
              crop.source_frame_id = input.source_frame_id;
              crop.source_timestamp_us = input.source_timestamp_us;
              crop.original_bbox_left = input.bbox_left;
              crop.original_bbox_top = input.bbox_top;
              crop.original_bbox_right = input.bbox_right;
              crop.original_bbox_bottom = input.bbox_bottom;
              crop.crop_bbox_ocr_left = ocr_crop_left;
              crop.crop_bbox_ocr_top = ocr_crop_top;
              crop.crop_bbox_ocr_right = ocr_crop_left + ocr_crop_width;
              crop.crop_bbox_ocr_bottom = ocr_crop_top + ocr_crop_height;
              crop.crop_bbox_left = src_crop_left;
              crop.crop_bbox_top = src_crop_top;
              crop.crop_bbox_right = src_crop_left + src_crop_width;
              crop.crop_bbox_bottom = src_crop_top + src_crop_height;
              crop.ocr_frame_width = ocr_w;
              crop.ocr_frame_height = ocr_h;
              crop.source_frame_width = src_w;
              crop.source_frame_height = src_h;
              crop.canonical_raster_width = options.canonical_raster_width;
              crop.canonical_raster_height = options.canonical_raster_height;
              crop.bbox_coordinate_space = "ocr_frame";
              crop.transform_scale_x = scale_x;
              crop.transform_scale_y = scale_y;
              crop.crop_extraction_method = "ffmpeg_crop_scaled_to_source";
              crop.evidence_quality = "unverified";
              crop.evidence_quality_reason = "Crop extracted but not mechanically verified to support linked observation (PP-OCR re-read not implemented for crops)";
              crop.roi_ocr_text = "";
              crop.roi_ocr_confidence = 0.0;
              crop.roi_ocr_word_count = 0;
              crop.frame_width = input.frame_width;
              crop.frame_height = input.frame_height;
              crop.crop_transform = "color";
              crop.image_format = options.crop_image_format;
              crop.crop_file_path = "text/evidence_crops/" + crop_filename;
              crop.crop_size_bytes = reduced_bytes;
              crop.blake3_hash = blake3_hash;

              if (input.detection_count > 1) {
                crop.selection_reason = "representative";
              } else if (input.confidence >= 0.5) {
                crop.selection_reason = "best_confidence";
              } else {
                crop.selection_reason = "first_detection";
              }

              result.crops.push_back(std::move(crop));
              total_crops++;
              report_input_processed(i);
              continue;
            }
          }
        }
      }

      std::error_code rm_ec;
      std::filesystem::remove(crop_path, rm_ec);
      crops_skipped++;
      skipped_by_bytes++;
      result.roi_ocr_results[i].succeeded = false;
      if (result.crops_skipped_reason.empty()) {
        result.crops_skipped_reason =
            "Total crop bytes cap reached (" +
            std::to_string(effective_byte_budget) +
            "); crop of " + std::to_string(crop_bytes) +
            " bytes would exceed remaining budget of " +
            std::to_string(effective_byte_budget - total_bytes) +
            " bytes";
      }
      report_input_processed(i);
      continue;
    }

    // No ROI hardening — crops are generated for visual evidence only
    result.roi_ocr_results[i].succeeded = false;

    std::string blake3_hash;
    try {
      blake3_hash = svp::models::blake3_hex_for_file(crop_path);
    } catch (...) {
      blake3_hash = "";
    }

    total_bytes += crop_bytes;

    EvidenceCropRecord crop;
    crop.crop_id = crop_id;
    crop.text_region_id = input.text_region_id;
    crop.text_observation_id = input.text_observation_id;
    crop.source_frame_id = input.source_frame_id;
    crop.source_timestamp_us = input.source_timestamp_us;
    crop.original_bbox_left = input.bbox_left;
    crop.original_bbox_top = input.bbox_top;
    crop.original_bbox_right = input.bbox_right;
    crop.original_bbox_bottom = input.bbox_bottom;
    crop.crop_bbox_ocr_left = ocr_crop_left;
    crop.crop_bbox_ocr_top = ocr_crop_top;
    crop.crop_bbox_ocr_right = ocr_crop_left + ocr_crop_width;
    crop.crop_bbox_ocr_bottom = ocr_crop_top + ocr_crop_height;
    crop.crop_bbox_left = src_crop_left;
    crop.crop_bbox_top = src_crop_top;
    crop.crop_bbox_right = src_crop_left + src_crop_width;
    crop.crop_bbox_bottom = src_crop_top + src_crop_height;
    crop.ocr_frame_width = ocr_w;
    crop.ocr_frame_height = ocr_h;
    crop.source_frame_width = src_w;
    crop.source_frame_height = src_h;
    crop.canonical_raster_width = options.canonical_raster_width;
    crop.canonical_raster_height = options.canonical_raster_height;
    crop.bbox_coordinate_space = "ocr_frame";
    crop.transform_scale_x = scale_x;
    crop.transform_scale_y = scale_y;
    crop.crop_extraction_method = "ffmpeg_crop_scaled_to_source";
    crop.evidence_quality = "unverified";
    crop.evidence_quality_reason = "Crop extracted but not mechanically verified to support linked observation (PP-OCR re-read not implemented for crops)";
    crop.roi_ocr_text = "";
    crop.roi_ocr_confidence = 0.0;
    crop.roi_ocr_word_count = 0;
    crop.frame_width = input.frame_width;
    crop.frame_height = input.frame_height;
    crop.crop_transform = "color";
    crop.image_format = options.crop_image_format;
    crop.crop_file_path = "text/evidence_crops/" + crop_filename;
    crop.crop_size_bytes = crop_bytes;
    crop.blake3_hash = blake3_hash;

    if (input.detection_count > 1) {
      crop.selection_reason = "representative";
    } else if (input.confidence >= 0.5) {
      crop.selection_reason = "best_confidence";
    } else {
      crop.selection_reason = "first_detection";
    }

    result.crops.push_back(std::move(crop));
    total_crops++;
    report_input_processed(i);
  }

  result.crop_count = static_cast<std::int64_t>(result.crops.size());
  result.total_crop_bytes = total_bytes;
  result.crops_skipped_count = static_cast<std::int64_t>(crops_skipped);
  result.crops_skipped_by_count_cap = skipped_by_count;
  result.crops_skipped_by_byte_cap = skipped_by_bytes;
  result.crops_skipped_by_extraction = skipped_by_extraction;
  result.every_observation_has_crop =
      (result.crops.size() == inputs.size()) && !inputs.empty();
  result.crop_coverage_status =
      result.every_observation_has_crop ? "full" : "partial";

  if (crops_skipped > 0 && result.crops_skipped_reason.empty()) {
    if (total_crops >= effective_max_crops) {
      result.crops_skipped_reason =
          "Total crop count cap reached (" +
          std::to_string(effective_max_crops) + ")";
    } else if (total_bytes >= effective_byte_budget) {
      result.crops_skipped_reason =
          "Total crop bytes cap reached (" +
          std::to_string(effective_byte_budget) + ")";
    } else {
      result.crops_skipped_reason = "Extraction failures";
    }
  }

  {
    const std::filesystem::path jsonl_path =
        staging_dir / "text" / "evidence_crops.jsonl";
    std::ofstream out(jsonl_path);
    if (out) {
      for (const auto& crop : result.crops) {
        out << evidence_crop_to_json(crop).dump() << "\n";
      }
    }
  }
  result.crops_written = true;

  return result;
}

}  // namespace svp::vision
