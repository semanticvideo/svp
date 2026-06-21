#include "svp/vision/evidence_crop.hpp"

#include "svp/models/hash.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
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

std::vector<std::string> split(const std::string& s, char delim) {
  std::vector<std::string> tokens;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, delim)) {
    tokens.push_back(item);
  }
  return tokens;
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

struct TesseractWord {
  std::string text;
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
  int confidence = 0;
};

std::vector<TesseractWord> parse_tesseract_tsv(const std::string& output) {
  std::vector<TesseractWord> words;
  std::istringstream iss(output);
  std::string line;
  bool header_seen = false;
  int left_col = -1, top_col = -1, width_col = -1, height_col = -1;
  int conf_col = -1, text_col = -1, level_col = -1;

  while (std::getline(iss, line)) {
    std::vector<std::string> cols = split(line, '\t');
    if (cols.empty()) continue;

    if (!header_seen) {
      for (std::size_t i = 0; i < cols.size(); ++i) {
        if (cols[i] == "level") level_col = static_cast<int>(i);
        else if (cols[i] == "left") left_col = static_cast<int>(i);
        else if (cols[i] == "top") top_col = static_cast<int>(i);
        else if (cols[i] == "width") width_col = static_cast<int>(i);
        else if (cols[i] == "height") height_col = static_cast<int>(i);
        else if (cols[i] == "conf") conf_col = static_cast<int>(i);
        else if (cols[i] == "text") text_col = static_cast<int>(i);
      }
      header_seen = true;
      continue;
    }

    if (level_col >= 0 && cols.size() > static_cast<std::size_t>(level_col)) {
      try {
        int level = std::stoi(cols[level_col]);
        if (level != 5) continue;
      } catch (...) {
        continue;
      }
    }

    if (text_col < 0 || static_cast<int>(cols.size()) <= text_col) continue;
    std::string text = trim(cols[text_col]);
    if (text.empty()) continue;

    TesseractWord word;
    word.text = text;
    try {
      if (left_col >= 0 && static_cast<int>(cols.size()) > left_col)
        word.left = std::stoi(cols[left_col]);
      if (top_col >= 0 && static_cast<int>(cols.size()) > top_col)
        word.top = std::stoi(cols[top_col]);
      if (width_col >= 0 && static_cast<int>(cols.size()) > width_col)
        word.right = word.left + std::stoi(cols[width_col]);
      if (height_col >= 0 && static_cast<int>(cols.size()) > height_col)
        word.bottom = word.top + std::stoi(cols[height_col]);
      if (conf_col >= 0 && static_cast<int>(cols.size()) > conf_col)
        word.confidence = std::stoi(cols[conf_col]);
    } catch (...) {
      continue;
    }
    words.push_back(std::move(word));
  }
  return words;
}

// Run tesseract on a crop image with a specific PSM.
std::vector<TesseractWord> run_tesseract_on_crop(
    const std::filesystem::path& tesseract_path,
    const std::filesystem::path& image_path,
    const std::string& language,
    int psm,
    std::string& error) {
  std::vector<TesseractWord> words;

  std::filesystem::path resolved = image_path;
  std::error_code canon_ec;
  auto canon = std::filesystem::canonical(image_path, canon_ec);
  if (!canon_ec) {
    resolved = canon;
  }

  std::string err_file = resolved.string() + ".err";
  std::string cmd =
      shell_quote(tesseract_path) +
      " " + shell_quote(resolved) +
      " stdout"
      " --psm " + std::to_string(psm) +
      " -l " + language +
      " tsv"
      " 2>" + shell_quote(err_file);

  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    error = "popen failed: " + std::string(std::strerror(errno));
    return words;
  }

  std::string output;
  char buffer[4096];
  while (true) {
    const std::size_t n = fread(buffer, 1, sizeof(buffer), pipe);
    if (n == 0) break;
    output.append(buffer, n);
  }
  const int status = pclose(pipe);
  const bool exit_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;

  if (std::filesystem::exists(err_file)) {
    std::error_code ec;
    std::filesystem::remove(err_file, ec);
  }

  if (!exit_ok) {
    error = "tesseract exited with status " +
            std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : status);
  }

  words = parse_tesseract_tsv(output);
  return words;
}

// Extract a crop from the source video at a given timestamp and bbox.
// Uses ffmpeg crop filter with optional preprocessing.
// Returns the path to the extracted crop image.
bool extract_crop_from_source(
    const std::filesystem::path& ffmpeg_path,
    const std::filesystem::path& source_path,
    std::int64_t seek_us,
    int crop_left, int crop_top, int crop_width, int crop_height,
    int target_width, int target_height,
    const std::string& preprocessing,
    const std::string& image_format,
    int jpeg_quality,
    const std::filesystem::path& output_path,
    std::string& error) {
  const std::string seek = microseconds_to_seek_string(seek_us);

  // Build the crop filter: crop=width:height:left:top
  std::string crop_filter =
      "crop=" + std::to_string(crop_width) + ":" +
      std::to_string(crop_height) + ":" +
      std::to_string(crop_left) + ":" +
      std::to_string(crop_top);

  // Scale up the crop for better OCR (2x, capped at 2000px wide)
  const int scaled_width = std::min(target_width * 2, 2000);
  const int scaled_height = static_cast<int>(
      std::round(static_cast<double>(target_height) * scaled_width /
                 std::max(1, target_width)));
  crop_filter += ",scale=" + std::to_string(scaled_width) + ":" +
                  std::to_string(scaled_height);

  // Apply preprocessing variant
  if (preprocessing == "grayscale_sharpen") {
    crop_filter += ",format=gray,unsharp=5:5:1.0";
  } else if (preprocessing == "grayscale_threshold") {
    crop_filter += ",format=gray,eq=contrast=1.5:brightness=0.0";
  } else if (preprocessing == "grayscale") {
    crop_filter += ",format=gray";
  }

  // Output format
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

// Build raw text from tesseract words and compute average confidence.
RoiOcrResult build_roi_ocr_result(
    const std::vector<TesseractWord>& words,
    const std::string& preprocessing,
    int psm) {
  RoiOcrResult result;
  result.preprocessing_variant = preprocessing;
  result.psm_used = psm;
  result.word_count = static_cast<int>(words.size());

  if (words.empty()) {
    result.succeeded = false;
    return result;
  }

  std::string raw_text;
  double conf_sum = 0.0;
  for (std::size_t i = 0; i < words.size(); ++i) {
    if (i > 0) raw_text += " ";
    raw_text += words[i].text;
    conf_sum += static_cast<double>(words[i].confidence);
  }

  result.raw_text = raw_text;
  result.confidence = conf_sum / static_cast<double>(words.size()) / 100.0;
  result.succeeded = true;
  return result;
}

// Run ROI-based Tesseract on a crop with multiple PSM modes.
// The crop image has already been extracted with grayscale_sharpen
// preprocessing by extract_crop_from_source. Here we only vary the
// Tesseract page segmentation mode (PSM) to find the best result.
RoiOcrResult run_roi_tesseract(
    const std::filesystem::path& tesseract_path,
    const std::filesystem::path& crop_image_path,
    const std::string& language) {
  RoiOcrResult best;

  // Try different PSM modes on the same preprocessed crop image.
  // PSM 6 = assume a single uniform block of text.
  // PSM 11 = sparse text, find as much text as possible.
  struct Variant {
    const char* name;
    int psm;
  };
  static const Variant variants[] = {
    {"psm6", 6},
    {"psm11", 11},
  };

  for (const auto& v : variants) {
    std::string err;
    auto words = run_tesseract_on_crop(
        tesseract_path, crop_image_path, language, v.psm, err);
    auto result = build_roi_ocr_result(words, v.name, v.psm);

    // Pick the result with the most words, then highest confidence
    if (!result.succeeded) continue;

    bool better = false;
    if (!best.succeeded) {
      better = true;
    } else if (result.word_count > best.word_count) {
      better = true;
    } else if (result.word_count == best.word_count &&
               result.confidence > best.confidence) {
      better = true;
    }

    if (better) {
      best = result;
    }
  }

  return best;
}

// Expand a bbox by a margin, clamped to frame dimensions.
void expand_bbox(
    int bbox_left, int bbox_top, int bbox_right, int bbox_bottom,
    int frame_width, int frame_height,
    double margin_ratio,
    int& crop_left, int& crop_top, int& crop_width, int& crop_height) {
  const int bw = bbox_right - bbox_left;
  const int bh = bbox_bottom - bbox_top;
  const int margin_x = static_cast<int>(bw * margin_ratio);
  const int margin_y = static_cast<int>(bh * margin_ratio);

  crop_left = std::max(0, bbox_left - margin_x);
  crop_top = std::max(0, bbox_top - margin_y);
  int crop_right = std::min(frame_width, bbox_right + margin_x);
  int crop_bottom = std::min(frame_height, bbox_bottom + margin_y);

  crop_width = crop_right - crop_left;
  crop_height = crop_bottom - crop_top;

  if (crop_width < 1) crop_width = 1;
  if (crop_height < 1) crop_height = 1;
}

// Read file size
std::int64_t get_file_size(const std::filesystem::path& path) {
  std::error_code ec;
  auto size = std::filesystem::file_size(path, ec);
  if (ec) return 0;
  return static_cast<std::int64_t>(size);
}

}  // namespace

nlohmann::json evidence_crop_to_json(const EvidenceCropRecord& record) {
  return {
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
      {"crop_bbox", {
          record.crop_bbox_left,
          record.crop_bbox_top,
          record.crop_bbox_right,
          record.crop_bbox_bottom
      }},
      {"frame_width", record.frame_width},
      {"frame_height", record.frame_height},
      {"crop_transform", record.crop_transform},
      {"image_format", record.image_format},
      {"crop_file_path", record.crop_file_path},
      {"crop_size_bytes", record.crop_size_bytes},
      {"blake3_hash", record.blake3_hash},
      {"selection_reason", record.selection_reason},
  };
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
      {"crops", crops_arr},
  };
}

EvidenceCropResult generate_evidence_crops_internal(
    const EvidenceCropOptions& options,
    const std::vector<CropGenerationInput>& inputs,
    const std::filesystem::path& staging_dir) {
  EvidenceCropResult result;

  const std::filesystem::path crops_dir = staging_dir / "text" / "evidence_crops";
  std::filesystem::create_directories(crops_dir);

  const std::string ext =
      (options.crop_image_format == "png") ? "png" : "jpg";

  std::int64_t total_bytes = 0;
  std::size_t total_crops = 0;
  std::size_t crops_skipped = 0;

  result.roi_ocr_results.resize(inputs.size());

  for (std::size_t i = 0; i < inputs.size(); ++i) {
    const auto& input = inputs[i];

    // Check total crop count cap
    if (total_crops >= options.max_total_crops) {
      crops_skipped++;
      result.roi_ocr_results[i].succeeded = false;
      continue;
    }

    // Check total bytes cap
    if (total_bytes >= options.max_total_crop_bytes) {
      crops_skipped++;
      result.roi_ocr_results[i].succeeded = false;
      continue;
    }

    // Expand bbox with 20% margin for context
    int crop_left, crop_top, crop_width, crop_height;
    expand_bbox(
        input.bbox_left, input.bbox_top,
        input.bbox_right, input.bbox_bottom,
        input.frame_width, input.frame_height,
        0.20,
        crop_left, crop_top, crop_width, crop_height);

    // Determine crop file name
    const std::string crop_id = pad_id("crop_", static_cast<int>(total_crops + 1));
    const std::string crop_filename = crop_id + "." + ext;
    const std::filesystem::path crop_path = crops_dir / crop_filename;

    // Extract the crop with grayscale_sharpen preprocessing
    std::string extract_error;
    bool crop_ok = extract_crop_from_source(
        options.ffmpeg_path, options.source_media_path,
        input.source_timestamp_us,
        crop_left, crop_top, crop_width, crop_height,
        crop_width, crop_height,
        "grayscale_sharpen",
        options.crop_image_format,
        options.jpeg_quality,
        crop_path, extract_error);

    if (!crop_ok) {
      crops_skipped++;
      result.roi_ocr_results[i].succeeded = false;
      continue;
    }

    // Measure the extracted crop file size.
    const std::int64_t crop_bytes = get_file_size(crop_path);

    // Enforce the byte cap as a hard upper bound: if accepting this
    // crop would push total_bytes over max_total_crop_bytes, skip it
    // and remove the orphaned file so no stray crops remain on disk.
    if (total_bytes + crop_bytes > options.max_total_crop_bytes) {
      std::error_code rm_ec;
      std::filesystem::remove(crop_path, rm_ec);
      crops_skipped++;
      result.roi_ocr_results[i].succeeded = false;
      if (result.crops_skipped_reason.empty()) {
        result.crops_skipped_reason =
            "Total crop bytes cap reached (" +
            std::to_string(options.max_total_crop_bytes) +
            "); crop of " + std::to_string(crop_bytes) +
            " bytes would exceed remaining budget of " +
            std::to_string(options.max_total_crop_bytes - total_bytes) +
            " bytes";
      }
      continue;
    }

    // Run ROI-based Tesseract hardening on the crop
    RoiOcrResult roi_result = run_roi_tesseract(
        options.tesseract_path, crop_path, options.language);
    result.roi_ocr_results[i] = roi_result;

    // Compute BLAKE3 hash of the crop file
    std::string blake3_hash;
    try {
      blake3_hash = svp::models::blake3_hex_for_file(crop_path);
    } catch (...) {
      blake3_hash = "";
    }

    total_bytes += crop_bytes;

    // Build the crop record
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
    crop.crop_bbox_left = crop_left;
    crop.crop_bbox_top = crop_top;
    crop.crop_bbox_right = crop_left + crop_width;
    crop.crop_bbox_bottom = crop_top + crop_height;
    crop.frame_width = input.frame_width;
    crop.frame_height = input.frame_height;
    crop.crop_transform = "grayscale_sharpen";
    crop.image_format = options.crop_image_format;
    crop.crop_file_path = "text/evidence_crops/" + crop_filename;
    crop.crop_size_bytes = crop_bytes;
    crop.blake3_hash = blake3_hash;

    // Selection reason
    if (input.detection_count > 1) {
      crop.selection_reason = "representative";
    } else if (input.confidence >= 0.5) {
      crop.selection_reason = "best_confidence";
    } else {
      crop.selection_reason = "first_detection";
    }

    result.crops.push_back(std::move(crop));
    total_crops++;
  }

  result.crop_count = static_cast<std::int64_t>(result.crops.size());
  result.total_crop_bytes = total_bytes;
  result.crops_skipped_count = static_cast<std::int64_t>(crops_skipped);

  if (crops_skipped > 0 && result.crops_skipped_reason.empty()) {
    if (total_crops >= options.max_total_crops) {
      result.crops_skipped_reason =
          "Total crop count cap reached (" +
          std::to_string(options.max_total_crops) + ")";
    } else if (total_bytes >= options.max_total_crop_bytes) {
      result.crops_skipped_reason =
          "Total crop bytes cap reached (" +
          std::to_string(options.max_total_crop_bytes) + ")";
    } else {
      result.crops_skipped_reason = "Extraction or OCR failures";
    }
  }

  // Write evidence_crops.jsonl
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
