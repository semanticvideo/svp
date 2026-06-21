#include "svp/vision/evidence_crop.hpp"

#include "svp/models/hash.hpp"

#include <algorithm>
#include <cctype>
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

  // Normalize crop dimensions for OCR and storage efficiency.
  // - Upscale small crops (below min_ocr_width) for better OCR accuracy.
  // - Downscale very large crops (above max_crop_width) to cap file size.
  // This is resolution-agnostic: works for SD, HD, 4K, 8K, or any source.
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

  // Apply preprocessing variant
  if (preprocessing == "grayscale_sharpen") {
    crop_filter += ",format=gray,unsharp=5:5:0.5";
  } else if (preprocessing == "grayscale_threshold") {
    crop_filter += ",format=gray,eq=contrast=1.5:brightness=0.0";
  } else if (preprocessing == "grayscale") {
    crop_filter += ",format=gray";
  } else if (preprocessing == "color") {
    // Keep color. Tesseract handles color internally, and color preserves
    // more visual information for audit purposes.
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
  // PSM 3 = fully automatic page segmentation (default, best for multi-line).
  // PSM 6 = assume a single uniform block of text.
  // PSM 7 = treat the image as a single text line.
  // PSM 11 = sparse text, find as much text as possible.
  struct Variant {
    const char* name;
    int psm;
  };
  static const Variant variants[] = {
    {"psm3", 3},
    {"psm6", 6},
    {"psm7", 7},
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

// Expand a text bbox into a centered same-line evidence window, clamped to
// frame dimensions. OCR boxes can cover only part of a handwritten line, but
// evidence crops need to preserve surrounding line context without assuming the
// missing context is on the left or right.
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

// Read file size
std::int64_t get_file_size(const std::filesystem::path& path) {
  std::error_code ec;
  auto size = std::filesystem::file_size(path, ec);
  if (ec) return 0;
  return static_cast<std::int64_t>(size);
}

// Extract only alphanumeric characters from a string (for text comparison).
std::string alphanumeric_only(const std::string& s) {
  std::string result;
  for (char c : s) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
      result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
  }
  return result;
}

// Check if two text strings share exact alphanumeric content.
// This intentionally avoids fuzzy prefix/suffix scoring because evidence
// quality should not mark a crop strong when OCR misreads important digits.
bool texts_share_content(const std::string& a, const std::string& b) {
  const std::string aa = alphanumeric_only(a);
  const std::string ab = alphanumeric_only(b);
  if (aa.empty() || ab.empty()) return false;

  const std::string& shorter = (aa.size() <= ab.size()) ? aa : ab;
  const std::string& longer = (aa.size() <= ab.size()) ? ab : aa;

  if (shorter.size() <= 2) return longer.find(shorter) != std::string::npos;

  return longer.find(shorter) != std::string::npos;
}

// Assess evidence quality by comparing ROI OCR text with the linked
// observation's text.
struct EvidenceQuality {
  std::string quality;
  std::string reason;
};

EvidenceQuality assess_evidence_quality(
    const RoiOcrResult& roi_result,
    const std::string& observation_raw_text) {
  if (observation_raw_text.empty()) {
    return {"not_checked", "No observation text available for comparison"};
  }
  if (!roi_result.succeeded || roi_result.word_count == 0) {
    return {"unsupported", "ROI OCR on saved crop produced no words"};
  }
  if (texts_share_content(roi_result.raw_text, observation_raw_text)) {
    return {"strong", "ROI OCR on saved crop reproduces or supports linked observation"};
  }
  return {"weak", "ROI OCR on saved crop does not reproduce linked observation text"};
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

  // Determine effective dimensions for coordinate transform.
  // OCR frame dimensions come from options (set by caller from media plan).
  // Source frame dimensions come from options (set by caller from media plan).
  // If source dimensions are not provided, fall back to OCR frame dimensions
  // (no scaling needed in that case: source == OCR resolution).
  const int ocr_w = options.ocr_frame_width > 0 ?
      options.ocr_frame_width : 0;
  const int ocr_h = options.ocr_frame_height > 0 ?
      options.ocr_frame_height : 0;
  const int src_w = options.source_frame_width > 0 ?
      options.source_frame_width : ocr_w;
  const int src_h = options.source_frame_height > 0 ?
      options.source_frame_height : ocr_h;

  // Compute scale factors from OCR-frame to source-frame coordinates.
  // When OCR runs at a lower resolution than the source video, bbox
  // coordinates must be scaled up before applying ffmpeg crop on the
  // source video. This is the root cause fix for the coordinate-space bug.
  const double scale_x = (ocr_w > 0) ?
      static_cast<double>(src_w) / static_cast<double>(ocr_w) : 1.0;
  const double scale_y = (ocr_h > 0) ?
      static_cast<double>(src_h) / static_cast<double>(ocr_h) : 1.0;

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

    // Step 1: Expand bbox with same-line context in OCR-frame coordinate space.
    // Use the OCR frame dimensions (input.frame_width/height) for clamping.
    const int clamp_w = (ocr_w > 0) ? ocr_w : input.frame_width;
    const int clamp_h = (ocr_h > 0) ? ocr_h : input.frame_height;

    int ocr_crop_left, ocr_crop_top, ocr_crop_width, ocr_crop_height;
    expand_text_line_bbox(
        input.bbox_left, input.bbox_top,
        input.bbox_right, input.bbox_bottom,
        clamp_w, clamp_h,
        ocr_crop_left, ocr_crop_top, ocr_crop_width, ocr_crop_height);

    // Step 2: Scale the expanded OCR-frame bbox to source-frame coordinates.
    // This is the critical fix: ffmpeg operates on the source video at its
    // native resolution, so crop coordinates must be in source-frame space.
    int src_crop_left = static_cast<int>(std::round(ocr_crop_left * scale_x));
    int src_crop_top = static_cast<int>(std::round(ocr_crop_top * scale_y));
    int src_crop_width = static_cast<int>(std::round(ocr_crop_width * scale_x));
    int src_crop_height = static_cast<int>(std::round(ocr_crop_height * scale_y));

    // Clamp to source frame dimensions
    src_crop_left = std::max(0, std::min(src_crop_left, src_w));
    src_crop_top = std::max(0, std::min(src_crop_top, src_h));
    src_crop_width = std::min(src_crop_width, src_w - src_crop_left);
    src_crop_height = std::min(src_crop_height, src_h - src_crop_top);

    if (src_crop_width < 1 || src_crop_height < 1) {
      crops_skipped++;
      result.roi_ocr_results[i].succeeded = false;
      continue;
    }

    // Determine crop file name
    const std::string crop_id = pad_id("crop_", static_cast<int>(total_crops + 1));
    const std::string crop_filename = crop_id + "." + ext;
    const std::filesystem::path crop_path = crops_dir / crop_filename;

    // Step 3: Extract the crop from the source video using source-frame
    // coordinates. The ffmpeg crop filter operates on the full-resolution
    // source video, so coordinates must be in source-frame space.
    // Use color preprocessing to preserve maximum visual information.
    std::string extract_error;
    bool crop_ok = extract_crop_from_source(
        options.ffmpeg_path, options.source_media_path,
        input.source_timestamp_us,
        src_crop_left, src_crop_top, src_crop_width, src_crop_height,
        src_crop_width, src_crop_height,
        "color",
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

    // Assess evidence quality by comparing ROI OCR text with observation text
    EvidenceQuality eq = assess_evidence_quality(roi_result, input.observation_raw_text);

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
    // Original bbox in OCR-frame coordinates
    crop.original_bbox_left = input.bbox_left;
    crop.original_bbox_top = input.bbox_top;
    crop.original_bbox_right = input.bbox_right;
    crop.original_bbox_bottom = input.bbox_bottom;
    // Crop bbox in OCR-frame space (after margin expansion)
    crop.crop_bbox_ocr_left = ocr_crop_left;
    crop.crop_bbox_ocr_top = ocr_crop_top;
    crop.crop_bbox_ocr_right = ocr_crop_left + ocr_crop_width;
    crop.crop_bbox_ocr_bottom = ocr_crop_top + ocr_crop_height;
    // Crop bbox in source-frame space (after coordinate transform)
    crop.crop_bbox_left = src_crop_left;
    crop.crop_bbox_top = src_crop_top;
    crop.crop_bbox_right = src_crop_left + src_crop_width;
    crop.crop_bbox_bottom = src_crop_top + src_crop_height;
    // Dimension metadata for auditing
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
    // Evidence quality
    crop.evidence_quality = eq.quality;
    crop.evidence_quality_reason = eq.reason;
    crop.roi_ocr_text = roi_result.raw_text;
    crop.roi_ocr_confidence = roi_result.confidence;
    crop.roi_ocr_word_count = roi_result.word_count;
    // Legacy compat
    crop.frame_width = input.frame_width;
    crop.frame_height = input.frame_height;
    crop.crop_transform = "color";
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
