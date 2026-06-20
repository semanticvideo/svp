#include "svp/vision/ocr_generation.hpp"

#include "svp/media/media_ingest_plan.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <regex>
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

bool executable_exists(const std::filesystem::path& exe) {
  if (exe.empty()) return false;
  if (exe.has_parent_path()) return std::filesystem::exists(exe);
  const char* path_env = std::getenv("PATH");
  if (!path_env) return false;
  std::string paths(path_env);
  std::size_t start = 0;
  while (start <= paths.size()) {
    const std::size_t end = paths.find(':', start);
    const std::string entry =
        paths.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!entry.empty() && std::filesystem::exists(std::filesystem::path(entry) / exe)) {
      return true;
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return false;
}

nlohmann::json make_ocr_processor_provenance(
    const std::string& id, const std::string& type,
    const std::string& version, const std::string& runtime,
    const std::string& status, const std::string& note) {
  return {
      {"id", id},
      {"processor_type", type},
      {"processor_version", version},
      {"runtime", runtime},
      {"execution_provider", "cpu"},
      {"model_refs", nlohmann::json::array()},
      {"status", status},
      {"note", note},
  };
}

struct TesseractWord {
  std::string text;
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
  int confidence = 0;
};

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

std::string lower(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return out;
}

std::string microseconds_to_seek_string(std::int64_t us) {
  const std::int64_t seconds = us / 1000000;
  const std::int64_t fraction = us % 1000000;
  std::ostringstream oss;
  oss << seconds << "." << std::setw(6) << std::setfill('0') << fraction;
  return oss.str();
}

std::string normalize_text(const std::string& raw) {
  std::string result = trim(raw);
  // Collapse multiple whitespace into single space
  std::string collapsed;
  bool prev_space = false;
  for (char c : result) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (!prev_space) collapsed += ' ';
      prev_space = true;
    } else {
      collapsed += c;
      prev_space = false;
    }
  }
  return lower(collapsed);
}

// Write a ColorRasterFrame to a temporary PNG file using ffmpeg.
// Used as a fallback when direct source extraction is not available.
bool write_frame_to_png(const ColorRasterFrame& frame,
                        const std::filesystem::path& ffmpeg_path,
                        const std::filesystem::path& output_png,
                        std::string& error) {
  const std::size_t pixel_count =
      static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height);
  const std::size_t raw_size = pixel_count * 3;

  std::string cmd =
      shell_quote(ffmpeg_path) +
      " -v error"
      " -f rawvideo"
      " -pix_fmt rgb24"
      " -s " + std::to_string(frame.width) + "x" + std::to_string(frame.height) +
      " -i pipe:0"
      " -vframes 1"
      " -y " + shell_quote(output_png) +
      " 2>/dev/null";

  FILE* pipe = popen(cmd.c_str(), "w");
  if (!pipe) {
    error = "popen failed: " + std::string(std::strerror(errno));
    return false;
  }

  const std::size_t written =
      fwrite(frame.pixels.data(), 1, raw_size, pipe);
  const int status = pclose(pipe);
  const bool exited_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;

  if (written != raw_size) {
    error = "short write to ffmpeg: " + std::to_string(written) + "/" +
            std::to_string(raw_size);
    return false;
  }
  if (!exited_ok) {
    error = "ffmpeg exited with status " +
            std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : status);
    return false;
  }
  return true;
}

// Extract a frame from the source video as a PNG file at a given timestamp.
// Uses ffmpeg directly to avoid the raw RGB round-trip that can lose data.
bool extract_frame_png_from_source(
    const std::filesystem::path& ffmpeg_path,
    const std::filesystem::path& source_path,
    std::int64_t seek_us,
    int target_width,
    int target_height,
    const std::filesystem::path& output_png,
    std::string& error) {
  const std::string seek = microseconds_to_seek_string(seek_us);

  std::string cmd =
      shell_quote(ffmpeg_path) +
      " -v error"
      " -ss " + seek +
      " -i " + shell_quote(source_path) +
      " -vf scale=" + std::to_string(target_width) + ":" +
      std::to_string(target_height) +
      " -vframes 1"
      " -y " + shell_quote(output_png) +
      " 2>/dev/null";

  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    error = "popen failed: " + std::string(std::strerror(errno));
    return false;
  }

  // Read and discard any output
  char buffer[4096];
  while (fread(buffer, 1, sizeof(buffer), pipe) > 0) {}

  const int status = pclose(pipe);
  const bool exited_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;

  if (!exited_ok) {
    error = "ffmpeg exited with status " +
            std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : status);
    return false;
  }

  if (!std::filesystem::exists(output_png)) {
    error = "ffmpeg did not produce output PNG file";
    return false;
  }

  return true;
}

// Run tesseract on an image file and parse TSV output.
// tesseract <image> stdout --psm 11 -l <lang> tsv
// PSM 11 = "sparse text - find as much text as possible in no particular order"
std::vector<TesseractWord> run_tesseract_tsv(
    const std::filesystem::path& tesseract_path,
    const std::filesystem::path& image_path,
    const std::string& language,
    std::string& error) {
  std::vector<TesseractWord> words;

  std::string cmd =
      shell_quote(tesseract_path) +
      " " + shell_quote(image_path) +
      " stdout"
      " --psm 11"
      " -l " + language +
      " tsv"
      " 2>/dev/null";

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
  const bool exited_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;

  if (!exited_ok) {
    error = "tesseract exited with status " +
            std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : status);
    // Still try to parse whatever output we got
  }

  // Parse TSV: level page block par line word left top width height conf text
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

    // Only accept word-level rows (level == 5)
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
      int w_left = 0, w_top = 0, w_width = 0, w_height = 0;
      if (left_col >= 0 && static_cast<int>(cols.size()) > left_col)
        w_left = std::stoi(cols[left_col]);
      if (top_col >= 0 && static_cast<int>(cols.size()) > top_col)
        w_top = std::stoi(cols[top_col]);
      if (width_col >= 0 && static_cast<int>(cols.size()) > width_col)
        w_width = std::stoi(cols[width_col]);
      if (height_col >= 0 && static_cast<int>(cols.size()) > height_col)
        w_height = std::stoi(cols[height_col]);
      word.left = w_left;
      word.top = w_top;
      word.right = w_left + w_width;
      word.bottom = w_top + w_height;
      if (conf_col >= 0 && static_cast<int>(cols.size()) > conf_col)
        word.confidence = std::stoi(cols[conf_col]);
    } catch (...) {
      continue;
    }

    words.push_back(std::move(word));
  }

  return words;
}

// Group words into text regions based on spatial proximity.
// Words on the same line (overlapping y-ranges) and close in x are grouped.
struct WordGroup {
  std::vector<TesseractWord> words;
  int left = 0, top = 0, right = 0, bottom = 0;
  double avg_confidence = 0.0;
};

std::vector<WordGroup> group_words(const std::vector<TesseractWord>& words) {
  if (words.empty()) return {};

  // Sort by top coordinate
  std::vector<TesseractWord> sorted = words;
  std::sort(sorted.begin(), sorted.end(),
            [](const TesseractWord& a, const TesseractWord& b) {
              return a.top < b.top;
            });

  std::vector<WordGroup> groups;
  WordGroup current;
  current.words.push_back(sorted[0]);
  current.left = sorted[0].left;
  current.top = sorted[0].top;
  current.right = sorted[0].right;
  current.bottom = sorted[0].bottom;

  for (std::size_t i = 1; i < sorted.size(); ++i) {
    const auto& w = sorted[i];
    // Check if this word is on the same line as the current group
    const int group_height = current.bottom - current.top;
    const int word_center_y = (w.top + w.bottom) / 2;
    const int group_center_y = (current.top + current.bottom) / 2;
    const int y_tolerance = std::max(group_height / 2, 10);

    if (std::abs(word_center_y - group_center_y) <= y_tolerance) {
      // Same line — add to current group
      current.words.push_back(w);
      current.left = std::min(current.left, w.left);
      current.top = std::min(current.top, w.top);
      current.right = std::max(current.right, w.right);
      current.bottom = std::max(current.bottom, w.bottom);
    } else {
      // New line — flush current group and start a new one
      groups.push_back(std::move(current));
      current = WordGroup{};
      current.words.push_back(w);
      current.left = w.left;
      current.top = w.top;
      current.right = w.right;
      current.bottom = w.bottom;
    }
  }
  if (!current.words.empty()) {
    groups.push_back(std::move(current));
  }

  // Compute average confidence for each group
  for (auto& g : groups) {
    double sum = 0.0;
    for (const auto& w : g.words) {
      sum += static_cast<double>(w.confidence);
    }
    g.avg_confidence = g.words.empty() ? 0.0 : sum / g.words.size();
  }

  return groups;
}

// Parse numeric values from recognized text.
// Looks for currency ($N.NN), decimal numbers, and integers in the text.
struct ParsedNumber {
  std::string raw_text;
  std::string normalized_text;
  std::string number_kind;
  std::string numeric_value;
  std::string unit;
  double confidence;
};

std::vector<ParsedNumber> parse_numeric_values(const std::string& raw_text,
                                                double confidence) {
  std::vector<ParsedNumber> results;

  // Currency: $N.NN or $N
  static const std::regex currency_re(R"(\$(\d+(?:\.\d+)?))");
  // Decimal: N.NN
  static const std::regex decimal_re(R"(\b(\d+\.\d+)\b)");
  // Integer: N (standalone, not part of decimal or currency)
  static const std::regex integer_re(R"(\b(\d+)\b)");

  std::string::const_iterator search_start = raw_text.cbegin();
  std::smatch match;

  // First pass: find currency matches
  std::vector<std::pair<std::size_t, std::size_t>> matched_ranges;
  search_start = raw_text.cbegin();
  while (std::regex_search(search_start, raw_text.cend(), match, currency_re)) {
    ParsedNumber num;
    num.raw_text = match[0].str();
    num.normalized_text = match[1].str();
    num.number_kind = "decimal";
    num.numeric_value = match[1].str();
    num.unit = "currency_unknown";
    num.confidence = confidence;
    results.push_back(std::move(num));

    std::size_t pos = match[0].first - raw_text.cbegin();
    std::size_t len = match[0].length();
    matched_ranges.push_back({pos, pos + len});
    search_start = match[0].second;
  }

  // Second pass: find decimal matches not overlapping with currency
  search_start = raw_text.cbegin();
  while (std::regex_search(search_start, raw_text.cend(), match, decimal_re)) {
    std::size_t pos = match[0].first - raw_text.cbegin();
    std::size_t end_pos = pos + match[0].length();

    bool overlaps = false;
    for (const auto& r : matched_ranges) {
      if (pos < r.second && end_pos > r.first) {
        overlaps = true;
        break;
      }
    }
    if (!overlaps) {
      ParsedNumber num;
      num.raw_text = match[0].str();
      num.normalized_text = match[0].str();
      num.number_kind = "decimal";
      num.numeric_value = match[0].str();
      num.confidence = confidence;
      results.push_back(std::move(num));
      matched_ranges.push_back({pos, end_pos});
    }
    search_start = match[0].second;
  }

  // Third pass: find standalone integers not overlapping with previous matches
  search_start = raw_text.cbegin();
  while (std::regex_search(search_start, raw_text.cend(), match, integer_re)) {
    std::size_t pos = match[0].first - raw_text.cbegin();
    std::size_t end_pos = pos + match[0].length();

    bool overlaps = false;
    for (const auto& r : matched_ranges) {
      if (pos < r.second && end_pos > r.first) {
        overlaps = true;
        break;
      }
    }
    if (!overlaps) {
      ParsedNumber num;
      num.raw_text = match[0].str();
      num.normalized_text = match[0].str();
      num.number_kind = "integer";
      num.numeric_value = match[0].str();
      num.confidence = confidence;
      results.push_back(std::move(num));
      matched_ranges.push_back({pos, end_pos});
    }
    search_start = match[0].second;
  }

  return results;
}

std::string pad_id(const std::string& prefix, int index, int width = 6) {
  std::ostringstream oss;
  oss << prefix << std::setw(width) << std::setfill('0') << index;
  return oss.str();
}

}  // namespace

OcrGenerationResult generate_ocr_observations(
    const OcrGenerationOptions& options,
    const DecodedCanonicalFrames& frame_input,
    const std::filesystem::path& staging_dir) {
  OcrGenerationResult result;

  result.ocr_available = executable_exists(options.tesseract_path);
  result.ocr_frame_input_available =
      frame_input.decoding_succeeded && !frame_input.frames.empty();

  // When media_plan is provided, decode higher-resolution frames for OCR.
  // The canonical raster (640x360) is too small for reliable text detection;
  // we decode at a larger size and normalize bounding boxes back to canonical
  // raster coordinates.
  DecodedCanonicalFrames ocr_frames;
  bool using_high_res_frames = false;
  if (options.media_plan != nullptr &&
      options.ocr_frame_width > 0 && options.ocr_frame_height > 0) {
    ocr_frames = decode_frames_at_resolution(
        *options.media_plan, options.ffmpeg_path,
        options.ocr_frame_width, options.ocr_frame_height, 5);
    if (ocr_frames.decoding_succeeded && !ocr_frames.frames.empty()) {
      using_high_res_frames = true;
      result.ocr_frame_input_available = true;
    }
  }

  // Use high-res frames if available, otherwise fall back to canonical frames.
  const DecodedCanonicalFrames& effective_frames =
      using_high_res_frames ? ocr_frames : frame_input;

  if (!result.ocr_available) {
    result.blocker = "Tesseract OCR engine not found at: " +
        options.tesseract_path.string();
    result.text_absence.schema_version = "svp-text-absence-v1";
    result.text_absence.ocr_required = true;
    result.text_absence.ocr_completed = false;
    result.text_absence.reason = "processor_failed";
    result.text_absence.provenance_id = "processor_ocr_detector_0001";
    result.processors.push_back(make_ocr_processor_provenance(
        "processor_ocr_detector_0001", "ocr_detector",
        "svp-vision-ocr-generation-v1", "not_executed",
        "not_run", result.blocker));
    result.processors.push_back(make_ocr_processor_provenance(
        "processor_ocr_recognizer_0001", "ocr_recognizer",
        "svp-vision-ocr-generation-v1", "not_executed",
        "not_run", "Tesseract not available"));
    result.processors.push_back(make_ocr_processor_provenance(
        "processor_numeric_parser_0001", "numeric_parser",
        "svp-vision-ocr-generation-v1", "deterministic_cpp",
        "not_run", "No OCR text to parse"));
    return result;
  }

  if (!result.ocr_frame_input_available) {
    if (!effective_frames.decoding_attempted) {
      result.blocker = "Frame decoding was not attempted; OCR is blocked: " +
          effective_frames.skipped_reason;
    } else if (!effective_frames.decoding_succeeded) {
      result.blocker = "Frame decoding failed; OCR is blocked: " +
          effective_frames.skipped_reason;
    } else {
      result.blocker = "No decoded frames available for OCR";
    }
    result.text_absence.schema_version = "svp-text-absence-v1";
    result.text_absence.ocr_required = true;
    result.text_absence.ocr_completed = false;
    result.text_absence.reason = "processor_failed";
    result.text_absence.provenance_id = "processor_ocr_detector_0001";
    result.processors.push_back(make_ocr_processor_provenance(
        "processor_ocr_detector_0001", "ocr_detector",
        "svp-vision-ocr-generation-v1", "tesseract_subprocess",
        "not_run", result.blocker));
    result.processors.push_back(make_ocr_processor_provenance(
        "processor_ocr_recognizer_0001", "ocr_recognizer",
        "svp-vision-ocr-generation-v1", "tesseract_subprocess",
        "not_run", result.blocker));
    result.processors.push_back(make_ocr_processor_provenance(
        "processor_numeric_parser_0001", "numeric_parser",
        "svp-vision-ocr-generation-v1", "deterministic_cpp",
        "not_run", "No OCR text to parse"));
    return result;
  }

  result.ocr_detection_run = true;

  // Create temp directory for frame images
  const std::filesystem::path temp_dir = staging_dir / ".ocr_temp";
  std::filesystem::create_directories(temp_dir);

  int region_counter = 0;
  int obs_counter = 0;
  int numeric_counter = 0;
  std::size_t total_observations = 0;

  for (std::size_t frame_idx = 0; frame_idx < effective_frames.frames.size(); ++frame_idx) {
    if (total_observations >= options.max_observations) break;

    const ColorRasterFrame& frame = effective_frames.frames[frame_idx];

    // Extract frame as PNG for tesseract.
    // When media_plan is available, extract directly from the source video
    // at the OCR resolution (avoids raw RGB round-trip issues).
    // Otherwise, fall back to writing decoded pixels via ffmpeg.
    const std::filesystem::path png_path =
        temp_dir / (frame.frame_id + ".png");
    std::string write_error;
    bool png_ok = false;
    if (options.media_plan != nullptr && using_high_res_frames) {
      png_ok = extract_frame_png_from_source(
          options.ffmpeg_path, options.media_plan->source_path,
          frame.timestamp_us,
          options.ocr_frame_width, options.ocr_frame_height,
          png_path, write_error);
    } else {
      png_ok = write_frame_to_png(
          frame, options.ffmpeg_path, png_path, write_error);
    }
    if (!png_ok) {
      continue;
    }

    // Run tesseract on the PNG
    std::string tesseract_error;
    std::vector<TesseractWord> words = run_tesseract_tsv(
        options.tesseract_path, png_path, options.language, tesseract_error);

    // Clean up temp PNG
    std::error_code ec;
    std::filesystem::remove(png_path, ec);

    if (words.empty()) continue;

    // Filter by confidence
    std::vector<TesseractWord> filtered;
    for (const auto& w : words) {
      if (w.confidence >= options.min_word_confidence) {
        filtered.push_back(w);
      }
    }
    if (filtered.empty()) continue;

    // Group words into text regions
    std::vector<WordGroup> groups = group_words(filtered);

    for (const auto& group : groups) {
      if (total_observations >= options.max_observations) break;

      // Build raw text from words in the group
      std::string raw_text;
      for (std::size_t i = 0; i < group.words.size(); ++i) {
        if (i > 0) raw_text += " ";
        raw_text += group.words[i].text;
      }

      const double conf = group.avg_confidence / 100.0;
      const int frame_w = frame.width;
      const int frame_h = frame.height;

      if (frame_w <= 0 || frame_h <= 0) continue;

      // Clamp bounding box to frame dimensions
      const int bx_min = std::max(0, group.left);
      const int by_min = std::max(0, group.top);
      const int bx_max = std::min(frame_w, group.right);
      const int by_max = std::min(frame_h, group.bottom);

      if (bx_max <= bx_min || by_max <= by_min) continue;

      ++region_counter;
      ++obs_counter;
      ++total_observations;

      const std::string region_id = pad_id("text_region_", region_counter);
      const std::string obs_id = pad_id("text_obs_", obs_counter);

      // Normalized coordinates are relative to the frame dimensions.
      // bbox_px is in canonical analysis raster pixels: scale from OCR frame
      // coordinates to canonical raster dimensions.
      const int raster_w = (options.canonical_raster_width > 0) ?
          options.canonical_raster_width : frame_w;
      const int raster_h = (options.canonical_raster_height > 0) ?
          options.canonical_raster_height : frame_h;

      TextRegionRecord region;
      region.text_region_id = region_id;
      region.observation_type = "text_detection";
      region.start_us = frame.timestamp_us;
      region.end_us = frame.timestamp_us;
      region.frame_start = static_cast<std::int64_t>(frame_idx);
      region.frame_end = static_cast<std::int64_t>(frame_idx);
      region.bbox_norm = {
          static_cast<double>(bx_min) / frame_w,
          static_cast<double>(by_min) / frame_h,
          static_cast<double>(bx_max) / frame_w,
          static_cast<double>(by_max) / frame_h,
      };
      region.bbox_px = {
          static_cast<int>(std::round(static_cast<double>(bx_min) * raster_w / frame_w)),
          static_cast<int>(std::round(static_cast<double>(by_min) * raster_h / frame_h)),
          static_cast<int>(std::round(static_cast<double>(bx_max) * raster_w / frame_w)),
          static_cast<int>(std::round(static_cast<double>(by_max) * raster_h / frame_h)),
      };
      region.confidence = conf;
      region.provenance_id = "processor_ocr_detector_0001";
      result.text_regions.push_back(region);

      TextObservationRecord obs;
      obs.text_observation_id = obs_id;
      obs.text_region_id = region_id;
      obs.observation_type = "text_recognition";
      obs.raw_text = raw_text;
      obs.normalized_text = normalize_text(raw_text);
      obs.language = nlohmann::json{
          {"primary", options.language}, {"script", "Latn"},
          {"mode", "single"}, {"confidence", conf}};
      obs.confidence = conf;
      obs.layout_class = "scene_text";
      obs.source_frame_ids = {frame.frame_id};
      obs.provenance_id = "processor_ocr_recognizer_0001";
      result.text_observations.push_back(obs);

      // Parse numeric values from recognized text
      auto numbers = parse_numeric_values(raw_text, conf);
      for (const auto& num : numbers) {
        ++numeric_counter;
        NumericValueRecord nv;
        nv.numeric_value_id = pad_id("numeric_value_", numeric_counter);
        nv.text_observation_id = obs_id;
        nv.text_region_id = region_id;
        nv.raw_text = num.raw_text;
        nv.normalized_text = num.normalized_text;
        nv.number_kind = num.number_kind;
        nv.numeric_value = num.numeric_value;
        nv.unit = num.unit.empty() ? std::optional<std::string>{} :
            std::optional<std::string>{num.unit};
        nv.confidence = num.confidence;
        nv.parse_rule = "svp-number-parser-v1";
        nv.provenance_id = "processor_numeric_parser_0001";
        result.numeric_values.push_back(nv);
      }
    }
  }

  // Clean up temp directory
  std::error_code ec;
  std::filesystem::remove_all(temp_dir, ec);

  result.ocr_recognition_run = true;
  result.text_region_count = static_cast<std::int64_t>(result.text_regions.size());
  result.text_observation_count = static_cast<std::int64_t>(result.text_observations.size());
  result.numeric_value_count = static_cast<std::int64_t>(result.numeric_values.size());

  // Build text absence record
  result.text_absence.schema_version = "svp-text-absence-v1";
  result.text_absence.ocr_required = true;
  result.text_absence.ocr_completed = true;
  result.text_absence.text_region_count = result.text_region_count;
  result.text_absence.text_observation_count = result.text_observation_count;
  result.text_absence.numeric_value_count = result.numeric_value_count;
  result.text_absence.reason =
      result.text_observations.empty() ? "no_text_detected" : "ocr_completed";
  result.text_absence.provenance_id = "processor_ocr_detector_0001";

  // Build processor provenance
  result.processors.push_back(make_ocr_processor_provenance(
      "processor_ocr_detector_0001", "ocr_detector",
      "svp-vision-ocr-generation-v1", "tesseract_subprocess",
      "completed",
      "Text detection via tesseract subprocess on " +
          std::to_string(effective_frames.frames.size()) +
          " decoded frame(s) at " +
          std::to_string(effective_frames.frames.empty() ? 0 : effective_frames.frames[0].width) +
          "x" +
          std::to_string(effective_frames.frames.empty() ? 0 : effective_frames.frames[0].height) +
          " resolution; detected " +
          std::to_string(result.text_region_count) + " text region(s)"));
  result.processors.push_back(make_ocr_processor_provenance(
      "processor_ocr_recognizer_0001", "ocr_recognizer",
      "svp-vision-ocr-generation-v1", "tesseract_subprocess",
      "completed",
      "Text recognition via tesseract subprocess; produced " +
          std::to_string(result.text_observation_count) +
          " text observation(s)"));
  result.processors.push_back(make_ocr_processor_provenance(
      "processor_numeric_parser_0001", "numeric_parser",
      "svp-vision-ocr-generation-v1", "deterministic_cpp",
      "completed",
      "Parsed " + std::to_string(result.numeric_value_count) +
          " numeric value(s) from recognized text"));

  // Write text artifacts to staging
  const std::filesystem::path text_dir = staging_dir / "text";
  std::filesystem::create_directories(text_dir);

  // Write text_regions.jsonl
  {
    std::ofstream out(text_dir / "text_regions.jsonl");
    if (!out) {
      result.blocker = "Failed to open text_regions.jsonl";
      return result;
    }
    for (const auto& reg : result.text_regions) {
      out << text_region_to_json(reg).dump() << "\n";
    }
  }
  result.text_regions_written = true;

  // Write text_observations.jsonl
  {
    std::ofstream out(text_dir / "text_observations.jsonl");
    if (!out) {
      result.blocker = "Failed to open text_observations.jsonl";
      return result;
    }
    for (const auto& obs : result.text_observations) {
      out << text_observation_to_json(obs).dump() << "\n";
    }
  }
  result.text_observations_written = true;

  // Write numeric_values.jsonl
  {
    std::ofstream out(text_dir / "numeric_values.jsonl");
    if (!out) {
      result.blocker = "Failed to open numeric_values.jsonl";
      return result;
    }
    for (const auto& nv : result.numeric_values) {
      out << numeric_value_to_json(nv).dump() << "\n";
    }
  }
  result.numeric_values_written = true;

  // Write text_absence.json
  {
    std::ofstream out(text_dir / "text_absence.json");
    if (!out) {
      result.blocker = "Failed to open text_absence.json";
      return result;
    }
    out << text_absence_to_json(result.text_absence).dump(2) << "\n";
  }
  result.text_absence_written = true;

  return result;
}

nlohmann::json ocr_generation_result_to_json(const OcrGenerationResult& result) {
  nlohmann::json regions_arr = nlohmann::json::array();
  for (const auto& reg : result.text_regions) {
    regions_arr.push_back(text_region_to_json(reg));
  }
  nlohmann::json obs_arr = nlohmann::json::array();
  for (const auto& obs : result.text_observations) {
    obs_arr.push_back(text_observation_to_json(obs));
  }
  nlohmann::json num_arr = nlohmann::json::array();
  for (const auto& nv : result.numeric_values) {
    num_arr.push_back(numeric_value_to_json(nv));
  }

  return {
      {"ocr_available", result.ocr_available},
      {"ocr_frame_input_available", result.ocr_frame_input_available},
      {"ocr_detection_run", result.ocr_detection_run},
      {"ocr_recognition_run", result.ocr_recognition_run},
      {"text_regions_written", result.text_regions_written},
      {"text_observations_written", result.text_observations_written},
      {"numeric_values_written", result.numeric_values_written},
      {"text_absence_written", result.text_absence_written},
      {"text_region_count", result.text_region_count},
      {"text_observation_count", result.text_observation_count},
      {"numeric_value_count", result.numeric_value_count},
      {"blocker", result.blocker},
      {"text_regions", regions_arr},
      {"text_observations", obs_arr},
      {"numeric_values", num_arr},
      {"text_absence", text_absence_to_json(result.text_absence)},
      {"processors", result.processors},
  };
}

}  // namespace svp::vision
