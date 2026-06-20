#include "svp/vision/ocr_generation.hpp"

#include "svp/media/media_ingest_plan.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
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
      {"id", sanitize_utf8(id)},
      {"processor_type", sanitize_utf8(type)},
      {"processor_version", sanitize_utf8(version)},
      {"runtime", sanitize_utf8(runtime)},
      {"execution_provider", "cpu"},
      {"model_refs", nlohmann::json::array()},
      {"status", sanitize_utf8(status)},
      {"note", sanitize_utf8(note)},
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
  // Remove spaces after punctuation so OCR variants reconcile:
  // "$19. 99" -> "$19.99", "June 20, 2026" -> "june 20,2026"
  std::string no_punct_space;
  for (std::size_t i = 0; i < collapsed.size(); ++i) {
    if (i > 0 && collapsed[i] == ' ' &&
        (collapsed[i - 1] == '.' || collapsed[i - 1] == ',' ||
         collapsed[i - 1] == ';' || collapsed[i - 1] == ':' ||
         collapsed[i - 1] == '$' || collapsed[i - 1] == '!')) {
      continue;
    }
    no_punct_space += collapsed[i];
  }
  return lower(no_punct_space);
}

std::string alphanumeric_key(const std::string& text) {
  std::string key;
  for (char c : text) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
      key += std::tolower(static_cast<unsigned char>(c));
    }
  }
  return key;
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

// Run ffmpeg to extract a single frame as PNG with a given filter chain.
bool run_ffmpeg_extract(
    const std::filesystem::path& ffmpeg_path,
    const std::filesystem::path& source_path,
    const std::string& seek,
    const std::string& vf_filter,
    const std::filesystem::path& output_png,
    std::string& error) {
  std::string cmd =
      shell_quote(ffmpeg_path) +
      " -v error"
      " -ss " + seek +
      " -i " + shell_quote(source_path) +
      " -vf " + shell_quote_str(vf_filter) +
      " -vframes 1"
      " -y " + shell_quote(output_png) +
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
      std::filesystem::exists(output_png);
  if (!ok) {
    error = trim(output);
    if (error.empty()) {
      error = "ffmpeg exited with status " +
              std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : status);
    }
  }
  return ok;
}

// Extract a frame from the source video as a PNG file at a given timestamp.
// Uses ffmpeg directly to avoid the raw RGB round-trip that can lose data.
// Tries preprocessing (grayscale, histeq, unsharp) first, then falls back
// to a simple scale if the filter chain is unavailable or fails.
bool extract_frame_png_from_source(
    const std::filesystem::path& ffmpeg_path,
    const std::filesystem::path& source_path,
    std::int64_t seek_us,
    int target_width,
    int target_height,
    const std::filesystem::path& output_png,
    std::string& error,
    std::string& selected_fallback) {
  const std::string seek = microseconds_to_seek_string(seek_us);
  const std::string scale_filter =
      "scale=" + std::to_string(target_width) + ":" +
      std::to_string(target_height);

  std::string last_ffmpeg_err;

  // Try with full preprocessing: scale + grayscale + histeq + unsharp
  const std::string full_filter =
      scale_filter + ",format=gray,histeq,unsharp=5:5:1.0";
  if (run_ffmpeg_extract(ffmpeg_path, source_path, seek, full_filter, output_png, last_ffmpeg_err)) {
    selected_fallback = "scale_grayscale_histeq_unsharp";
    return true;
  }

  // Fallback 1: scale + grayscale only (histeq/unsharp may be unavailable)
  const std::string gray_filter = scale_filter + ",format=gray";
  if (run_ffmpeg_extract(ffmpeg_path, source_path, seek, gray_filter, output_png, last_ffmpeg_err)) {
    selected_fallback = "scale_grayscale";
    return true;
  }

  // Fallback 2: plain scale (no preprocessing at all)
  if (run_ffmpeg_extract(ffmpeg_path, source_path, seek, scale_filter, output_png, last_ffmpeg_err)) {
    selected_fallback = "scale_only";
    return true;
  }

  error = "ffmpeg failed to extract frame with all filter chains. Last error: " + last_ffmpeg_err;
  selected_fallback = "failed";
  return false;
}

// Parse tesseract TSV output into words.
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

struct TesseractRunDiagnostics {
  int psm = 0;
  bool exit_ok = false;
  int exit_status = -1;
  std::string error;
  std::size_t word_count = 0;
  bool tsv_available = false;
  std::string stderr_msg;
};

// Run tesseract with a specific PSM mode and parse TSV output.
std::vector<TesseractWord> run_tesseract_psm(
    const std::filesystem::path& tesseract_path,
    const std::filesystem::path& image_path,
    const std::string& language,
    int psm,
    std::string& error,
    int& exit_status,
    bool& exit_ok,
    bool& tsv_available,
    std::string& stderr_msg) {
  std::vector<TesseractWord> words;

  std::string err_file = image_path.string() + ".err";
  std::string cmd =
      shell_quote(tesseract_path) +
      " " + shell_quote(image_path) +
      " stdout"
      " --psm " + std::to_string(psm) +
      " -l " + language +
      " tsv"
      " 2>" + shell_quote(err_file);

  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    error = "popen failed: " + std::string(std::strerror(errno));
    exit_ok = false;
    exit_status = -1;
    tsv_available = false;
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
  exit_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : status;
  tsv_available = !output.empty();

  if (std::filesystem::exists(err_file)) {
    std::ifstream err_in(err_file);
    std::stringstream err_ss;
    err_ss << err_in.rdbuf();
    stderr_msg = trim(err_ss.str());
    std::error_code ec;
    std::filesystem::remove(err_file, ec);
  }

  if (!exit_ok) {
    error = "tesseract exited with status " +
            std::to_string(exit_status);
    if (!stderr_msg.empty()) {
      error += ": " + stderr_msg;
    }
  }

  return parse_tesseract_tsv(output);
}

// Run tesseract on an image file with PSM fallback.
// Tries PSM 11 (sparse text), then PSM 6 (uniform block), then PSM 3 (auto).
// Merges results, deduplicating by (text, left, top) to avoid double-counting.
std::vector<TesseractWord> run_tesseract_tsv(
    const std::filesystem::path& tesseract_path,
    const std::filesystem::path& image_path,
    const std::string& language,
    std::string& error,
    std::vector<TesseractRunDiagnostics>& diag_runs) {
  std::vector<TesseractWord> all_words;

  // PSM modes to try in order: sparse, uniform block, fully automatic
  const int psm_modes[] = {11, 6, 3};
  std::string last_error;

  for (int psm : psm_modes) {
    std::string psm_error;
    int exit_status = -1;
    bool exit_ok = false;
    bool tsv_available = false;
    std::string stderr_msg;
    auto psm_words = run_tesseract_psm(
        tesseract_path, image_path, language, psm, psm_error,
        exit_status, exit_ok, tsv_available, stderr_msg);

    TesseractRunDiagnostics r_diag;
    r_diag.psm = psm;
    r_diag.exit_ok = exit_ok;
    r_diag.exit_status = exit_status;
    r_diag.error = psm_error;
    r_diag.word_count = psm_words.size();
    r_diag.tsv_available = tsv_available;
    r_diag.stderr_msg = stderr_msg;
    diag_runs.push_back(r_diag);

    if (!psm_error.empty()) last_error = psm_error;

    for (auto& w : psm_words) {
      // Deduplicate: skip if same text at same position already exists
      bool dup = false;
      for (const auto& existing : all_words) {
        if (existing.text == w.text &&
            existing.left == w.left && existing.top == w.top) {
          dup = true;
          break;
        }
      }
      if (!dup) all_words.push_back(std::move(w));
    }

    // If we got good results from sparse mode, don't need fallback
    if (!all_words.empty() && psm == 11) break;
  }

  if (all_words.empty() && !last_error.empty()) {
    error = last_error;
  }

  return all_words;
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
// Only emits currency values, decimals, and integers that appear in date-like
// or multi-word context. Standalone bare integers from noisy single-word OCR
// are suppressed to avoid numeric noise.
struct ParsedNumber {
  std::string raw_text;
  std::string normalized_text;
  std::string number_kind;
  std::string numeric_value;
  std::string unit;
  double confidence;
};

// Check if the text looks like a date pattern (e.g. "June 20, 2026").
bool looks_like_date_context(const std::string& text) {
  static const std::regex date_re(
      R"((january|february|march|april|may|june|july|august|september|october|november|december)\s+\d{1,2}(?:,\s*|\s+)\d{2,4})",
      std::regex_constants::icase);
  return std::regex_search(text, date_re);
}

// Count the number of word tokens in the text.
int count_words(const std::string& text) {
  std::istringstream iss(text);
  std::string word;
  int count = 0;
  while (iss >> word) ++count;
  return count;
}

std::vector<ParsedNumber> parse_numeric_values(const std::string& raw_text,
                                                double confidence) {
  std::vector<ParsedNumber> results;

  // Currency: $N.NN, $N, or OCR variant "$N. NN" (space before cents)
  static const std::regex currency_re(R"(\$(\d+)(?:\.\s*(\d+))?)");
  // Decimal: N.NN
  static const std::regex decimal_re(R"(\b(\d+\.\d+)\b)");
  // Integer: N (standalone, not part of decimal or currency)
  static const std::regex integer_re(R"(\b(\d+)\b)");

  std::string::const_iterator search_start = raw_text.cbegin();
  std::smatch match;

  // First pass: find currency matches (always emitted — high signal)
  std::vector<std::pair<std::size_t, std::size_t>> matched_ranges;
  search_start = raw_text.cbegin();
  while (std::regex_search(search_start, raw_text.cend(), match, currency_re)) {
    ParsedNumber num;
    num.raw_text = match[0].str();
    // Normalize: remove spaces from the numeric value
    std::string dollars = match[1].str();
    std::string cents = match[2].str();
    if (!cents.empty()) {
      num.normalized_text = dollars + "." + cents;
      num.numeric_value = dollars + "." + cents;
    } else {
      num.normalized_text = dollars;
      num.numeric_value = dollars;
    }
    num.number_kind = "decimal";
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

  // Third pass: find standalone integers — only emit if in date context
  // (suppressing standalone low-context integer noise).
  const bool is_date = looks_like_date_context(raw_text);
  const bool allow_integers = is_date;

  if (allow_integers) {
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
  }

  return results;
}

std::string pad_id(const std::string& prefix, int index, int width = 6) {
  std::ostringstream oss;
  oss << prefix << std::setw(width) << std::setfill('0') << index;
  return oss.str();
}

// --- Multi-frame reconciliation structures ---

// A per-frame detection: one group of words found in a single frame.
struct FrameDetection {
  std::string frame_id;
  std::int64_t timestamp_us = 0;
  std::size_t frame_index = 0;
  int frame_width = 0;
  int frame_height = 0;
  std::string raw_text;
  double confidence = 0.0;
  int bbox_left = 0, bbox_top = 0, bbox_right = 0, bbox_bottom = 0;
};

// A reconciled observation: merged from multiple FrameDetections with the same
// normalized text and overlapping spatial position.
struct ReconciledObservation {
  std::string raw_text;
  std::string normalized_text;
  double confidence = 0.0;
  std::int64_t start_us = 0;
  std::int64_t end_us = 0;
  std::size_t frame_start = 0;
  std::size_t frame_end = 0;
  std::vector<std::string> source_frame_ids;
  int bbox_left = 0, bbox_top = 0, bbox_right = 0, bbox_bottom = 0;
  int frame_width = 0;
  int frame_height = 0;
  int detection_count = 1;
};

// Compute IoU (intersection over union) of two bounding boxes.
double bbox_iou(int a_left, int a_top, int a_right, int a_bottom,
                int b_left, int b_top, int b_right, int b_bottom) {
  const int inter_left = std::max(a_left, b_left);
  const int inter_top = std::max(a_top, b_top);
  const int inter_right = std::min(a_right, b_right);
  const int inter_bottom = std::min(a_bottom, b_bottom);
  if (inter_right <= inter_left || inter_bottom <= inter_top) return 0.0;
  const double inter_area =
      static_cast<double>(inter_right - inter_left) *
      static_cast<double>(inter_bottom - inter_top);
  const double a_area =
      static_cast<double>(a_right - a_left) *
      static_cast<double>(a_bottom - a_top);
  const double b_area =
      static_cast<double>(b_right - b_left) *
      static_cast<double>(b_bottom - b_top);
  const double union_area = a_area + b_area - inter_area;
  return union_area > 0.0 ? inter_area / union_area : 0.0;
}

// Reconcile per-frame detections into time-spanning observations.
// Detections with the same normalized text and overlapping bounding boxes
// (IoU > 0.3) are merged. Single-frame detections with low confidence are
// suppressed as likely OCR noise. Detections with very short text (<=2 chars)
// are filtered as noise.
std::vector<ReconciledObservation> reconcile_detections(
    const std::vector<FrameDetection>& detections,
    int total_frames,
    double min_confidence = 0.30,
    std::size_t min_text_chars = 3) {
  std::vector<ReconciledObservation> reconciled;

  // Group detections by alphanumeric key (so spacing/punctuation variants
  // like "$19. 99" and "1999" can reconcile together)
  std::map<std::string, std::vector<std::size_t>> by_text_key;
  for (std::size_t i = 0; i < detections.size(); ++i) {
    const std::string norm = normalize_text(detections[i].raw_text);
    const std::string key = alphanumeric_key(norm);

    // Skip very short text or punctuation-only noise based on alphanumeric content length
    if (key.length() < min_text_chars) continue;

    by_text_key[key].push_back(i);
  }

  for (const auto& [key, indices] : by_text_key) {
    // Cluster detections by spatial overlap
    std::vector<std::vector<std::size_t>> clusters;
    for (std::size_t idx : indices) {
      bool added = false;
      for (auto& cluster : clusters) {
        for (std::size_t cidx : cluster) {
          const double iou = bbox_iou(
              detections[idx].bbox_left, detections[idx].bbox_top,
              detections[idx].bbox_right, detections[idx].bbox_bottom,
              detections[cidx].bbox_left, detections[cidx].bbox_top,
              detections[cidx].bbox_right, detections[cidx].bbox_bottom);
          if (iou > 0.3) {
            cluster.push_back(idx);
            added = true;
            break;
          }
        }
        if (added) break;
      }
      if (!added) clusters.push_back({idx});
    }

    for (const auto& cluster : clusters) {
      ReconciledObservation obs;
      // Use the longest raw_text variant as the representative raw text
      std::size_t best_idx = cluster[0];
      for (std::size_t idx : cluster) {
        if (detections[idx].raw_text.size() > detections[best_idx].raw_text.size()) {
          best_idx = idx;
        }
      }
      obs.raw_text = detections[best_idx].raw_text;
      obs.normalized_text = normalize_text(obs.raw_text);
      obs.frame_width = detections[cluster[0]].frame_width;
      obs.frame_height = detections[cluster[0]].frame_height;
      obs.detection_count = static_cast<int>(cluster.size());

      double conf_sum = 0.0;
      obs.bbox_left = INT_MAX;
      obs.bbox_top = INT_MAX;
      obs.bbox_right = 0;
      obs.bbox_bottom = 0;
      obs.start_us = INT64_MAX;
      obs.end_us = 0;
      obs.frame_start = SIZE_MAX;
      obs.frame_end = 0;

      for (std::size_t idx : cluster) {
        const auto& det = detections[idx];
        conf_sum += det.confidence;
        obs.bbox_left = std::min(obs.bbox_left, det.bbox_left);
        obs.bbox_top = std::min(obs.bbox_top, det.bbox_top);
        obs.bbox_right = std::max(obs.bbox_right, det.bbox_right);
        obs.bbox_bottom = std::max(obs.bbox_bottom, det.bbox_bottom);
        obs.start_us = std::min(obs.start_us, det.timestamp_us);
        obs.end_us = std::max(obs.end_us, det.timestamp_us);
        obs.frame_start = std::min(obs.frame_start, det.frame_index);
        obs.frame_end = std::max(obs.frame_end, det.frame_index);
        obs.source_frame_ids.push_back(det.frame_id);
      }

      obs.confidence = conf_sum / static_cast<double>(cluster.size());

      // Suppress single-frame low-confidence detections as likely noise
      // (only if we have multiple frames — with 1 frame total, keep everything)
      if (total_frames > 1 && cluster.size() == 1 && obs.confidence < min_confidence) {
        continue;
      }

      // Boost confidence for observations detected across multiple frames
      if (cluster.size() > 1) {
        obs.confidence = std::min(1.0, obs.confidence + 0.1 * (cluster.size() - 1));
      }

      reconciled.push_back(std::move(obs));
    }
  }

  // Sort by start_us for stable output
  std::sort(reconciled.begin(), reconciled.end(),
            [](const ReconciledObservation& a, const ReconciledObservation& b) {
              return a.start_us < b.start_us;
            });

  return reconciled;
}

void write_failure_stage_files(const std::filesystem::path& staging_dir, const TextAbsenceRecord& text_absence) {
  const std::filesystem::path text_dir = staging_dir / "text";
  std::filesystem::create_directories(text_dir);

  {
    std::ofstream out(text_dir / "text_regions.jsonl");
  }
  {
    std::ofstream out(text_dir / "text_observations.jsonl");
  }
  {
    std::ofstream out(text_dir / "numeric_values.jsonl");
  }
  {
    std::ofstream out(text_dir / "text_absence.json");
    if (out) {
      out << text_absence_to_json(text_absence).dump(2) << "\n";
    }
  }
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

    write_failure_stage_files(staging_dir, result.text_absence);
    result.text_regions_written = true;
    result.text_observations_written = true;
    result.numeric_values_written = true;
    result.text_absence_written = true;

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

    write_failure_stage_files(staging_dir, result.text_absence);
    result.text_regions_written = true;
    result.text_observations_written = true;
    result.numeric_values_written = true;
    result.text_absence_written = true;

    return result;
  }

  result.ocr_detection_run = true;

  // Create temp directory for frame images
  const std::filesystem::path temp_dir = staging_dir / ".ocr_temp";
  std::filesystem::create_directories(temp_dir);

  // Phase 1: Collect per-frame detections
  std::vector<FrameDetection> all_detections;
  std::vector<nlohmann::json> frame_diagnostics;
  bool any_frame_failed = false;
  std::string failure_reason_details;

  for (std::size_t frame_idx = 0; frame_idx < effective_frames.frames.size(); ++frame_idx) {
    const ColorRasterFrame& frame = effective_frames.frames[frame_idx];

    // Extract frame as PNG for tesseract.
    const std::filesystem::path png_path =
        temp_dir / (frame.frame_id + ".png");
    std::string write_error;
    bool png_ok = false;
    std::string selected_fallback = "none";
    if (options.media_plan != nullptr && using_high_res_frames) {
      png_ok = extract_frame_png_from_source(
          options.ffmpeg_path, options.media_plan->source_path,
          frame.timestamp_us,
          options.ocr_frame_width, options.ocr_frame_height,
          png_path, write_error, selected_fallback);
    } else {
      png_ok = write_frame_to_png(
          frame, options.ffmpeg_path, png_path, write_error);
      if (png_ok) {
        selected_fallback = "write_frame_to_png";
      }
    }
    if (!png_ok) {
      nlohmann::json diag = {
          {"frame_id", sanitize_utf8(frame.frame_id)},
          {"timestamp_us", frame.timestamp_us},
          {"extraction_succeeded", false},
          {"extraction_error", sanitize_utf8(write_error)},
          {"extraction_path", sanitize_utf8(png_path.string())},
          {"preprocessing_fallback", sanitize_utf8(selected_fallback)},
          {"tesseract_attempted", false}
      };
      frame_diagnostics.push_back(diag);
      any_frame_failed = true;
      if (failure_reason_details.empty()) {
        failure_reason_details = sanitize_utf8("Frame extraction failed for " + frame.frame_id + ": " + write_error);
      }
      continue;
    }

    // Run tesseract on the PNG with PSM fallback
    std::vector<TesseractRunDiagnostics> diag_runs;
    std::string tesseract_error;
    std::vector<TesseractWord> words = run_tesseract_tsv(
        options.tesseract_path, png_path, options.language, tesseract_error, diag_runs);

    // Clean up temp PNG
    std::error_code ec;
    std::filesystem::remove(png_path, ec);

    // Check if Tesseract failed on all runs
    bool tesseract_failed_on_all = true;
    for (const auto& r : diag_runs) {
      if (r.exit_ok) {
        tesseract_failed_on_all = false;
        break;
      }
    }

    nlohmann::json t_runs_json = nlohmann::json::array();
    for (const auto& r : diag_runs) {
      t_runs_json.push_back({
          {"psm", r.psm},
          {"exit_ok", r.exit_ok},
          {"exit_status", r.exit_status},
          {"error", sanitize_utf8(r.error)},
          {"word_count", r.word_count},
          {"tsv_available", r.tsv_available},
          {"stderr_msg", sanitize_utf8(r.stderr_msg)}
      });
    }

    nlohmann::json diag = {
        {"frame_id", sanitize_utf8(frame.frame_id)},
        {"timestamp_us", frame.timestamp_us},
        {"extraction_succeeded", true},
        {"extraction_path", sanitize_utf8(png_path.string())},
        {"preprocessing_fallback", sanitize_utf8(selected_fallback)},
        {"tesseract_attempted", true},
        {"tesseract_succeeded", !tesseract_failed_on_all},
        {"tesseract_error", sanitize_utf8(tesseract_error)},
        {"psm_runs", t_runs_json}
    };
    frame_diagnostics.push_back(diag);

    if (tesseract_failed_on_all) {
      any_frame_failed = true;
      if (failure_reason_details.empty()) {
        failure_reason_details = sanitize_utf8("Tesseract failed on frame " + frame.frame_id + ": " + tesseract_error);
      }
      continue;
    }

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

      FrameDetection det;
      det.frame_id = frame.frame_id;
      det.timestamp_us = frame.timestamp_us;
      det.frame_index = frame_idx;
      det.frame_width = frame_w;
      det.frame_height = frame_h;
      det.raw_text = raw_text;
      det.confidence = conf;
      det.bbox_left = bx_min;
      det.bbox_top = by_min;
      det.bbox_right = bx_max;
      det.bbox_bottom = by_max;
      all_detections.push_back(std::move(det));
    }
  }

  // Clean up temp directory
  std::error_code ec;
  std::filesystem::remove_all(temp_dir, ec);

  // Surface any extraction/Tesseract failure as a blocker/error
  if (any_frame_failed) {
    result.blocker = "OCR processing failed: " + failure_reason_details;
    result.text_absence.schema_version = "svp-text-absence-v1";
    result.text_absence.ocr_required = true;
    result.text_absence.ocr_completed = false;
    result.text_absence.reason = "processor_failed";
    result.text_absence.provenance_id = "processor_ocr_detector_0001";
    result.text_absence.text_region_count = 0;
    result.text_absence.text_observation_count = 0;
    result.text_absence.numeric_value_count = 0;

    nlohmann::json detector_proc = make_ocr_processor_provenance(
        "processor_ocr_detector_0001", "ocr_detector",
        "svp-vision-ocr-generation-v1", "tesseract_subprocess",
        "failed",
        "Text detection failed: " + failure_reason_details);
    detector_proc["diagnostics"] = frame_diagnostics;
    result.processors.push_back(detector_proc);

    nlohmann::json recognizer_proc = make_ocr_processor_provenance(
        "processor_ocr_recognizer_0001", "ocr_recognizer",
        "svp-vision-ocr-generation-v1", "tesseract_subprocess",
        "failed",
        "Text recognition failed: " + failure_reason_details);
    recognizer_proc["diagnostics"] = frame_diagnostics;
    result.processors.push_back(recognizer_proc);

    result.processors.push_back(make_ocr_processor_provenance(
        "processor_numeric_parser_0001", "numeric_parser",
        "svp-vision-ocr-generation-v1", "deterministic_cpp",
        "not_run", "No OCR text to parse due to OCR execution failure"));

    // Write empty files to staging
    const std::filesystem::path text_dir = staging_dir / "text";
    std::filesystem::create_directories(text_dir);

    {
      std::ofstream out(text_dir / "text_regions.jsonl");
    }
    result.text_regions_written = true;

    {
      std::ofstream out(text_dir / "text_observations.jsonl");
    }
    result.text_observations_written = true;

    {
      std::ofstream out(text_dir / "numeric_values.jsonl");
    }
    result.numeric_values_written = true;

    {
      std::ofstream out(text_dir / "text_absence.json");
      if (out) {
        out << text_absence_to_json(result.text_absence).dump(2) << "\n";
      }
    }
    result.text_absence_written = true;

    return result;
  }

  // Phase 2: Reconcile detections across frames
  const int total_frames = static_cast<int>(effective_frames.frames.size());
  auto reconciled = reconcile_detections(all_detections, total_frames);

  // Phase 3: Emit reconciled observations as records
  int region_counter = 0;
  int obs_counter = 0;
  int numeric_counter = 0;

  for (const auto& robs : reconciled) {
    if (static_cast<std::size_t>(obs_counter) >= options.max_observations) break;

    ++region_counter;
    ++obs_counter;

    const std::string region_id = pad_id("text_region_", region_counter);
    const std::string obs_id = pad_id("text_obs_", obs_counter);

    const int frame_w = robs.frame_width;
    const int frame_h = robs.frame_height;
    const int raster_w = (options.canonical_raster_width > 0) ?
        options.canonical_raster_width : frame_w;
    const int raster_h = (options.canonical_raster_height > 0) ?
        options.canonical_raster_height : frame_h;

    TextRegionRecord region;
    region.text_region_id = region_id;
    region.observation_type = "text_detection";
    region.start_us = robs.start_us;
    region.end_us = robs.end_us;
    region.frame_start = static_cast<std::int64_t>(robs.frame_start);
    region.frame_end = static_cast<std::int64_t>(robs.frame_end);
    region.bbox_norm = {
        static_cast<double>(robs.bbox_left) / frame_w,
        static_cast<double>(robs.bbox_top) / frame_h,
        static_cast<double>(robs.bbox_right) / frame_w,
        static_cast<double>(robs.bbox_bottom) / frame_h,
    };
    region.bbox_px = {
        static_cast<int>(std::round(static_cast<double>(robs.bbox_left) * raster_w / frame_w)),
        static_cast<int>(std::round(static_cast<double>(robs.bbox_top) * raster_h / frame_h)),
        static_cast<int>(std::round(static_cast<double>(robs.bbox_right) * raster_w / frame_w)),
        static_cast<int>(std::round(static_cast<double>(robs.bbox_bottom) * raster_h / frame_h)),
    };
    region.confidence = robs.confidence;
    region.provenance_id = "processor_ocr_detector_0001";
    result.text_regions.push_back(region);

    TextObservationRecord obs;
    obs.text_observation_id = obs_id;
    obs.text_region_id = region_id;
    obs.observation_type = "text_recognition";
    obs.raw_text = robs.raw_text;
    obs.normalized_text = robs.normalized_text;
    obs.language = nlohmann::json{
        {"primary", options.language}, {"script", "Latn"},
        {"mode", "multi"}, {"confidence", robs.confidence}};
    obs.confidence = robs.confidence;
    obs.layout_class = "scene_text";
    obs.source_frame_ids = robs.source_frame_ids;
    obs.provenance_id = "processor_ocr_recognizer_0001";
    result.text_observations.push_back(obs);

    // Parse numeric values from recognized text
    auto numbers = parse_numeric_values(robs.raw_text, robs.confidence);
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
  nlohmann::json detector_proc = make_ocr_processor_provenance(
      "processor_ocr_detector_0001", "ocr_detector",
      "svp-vision-ocr-generation-v1", "tesseract_subprocess",
      "completed",
      "Text detection via tesseract subprocess (PSM 11/6/3 fallback) on " +
          std::to_string(effective_frames.frames.size()) +
          " decoded frame(s) at " +
          std::to_string(effective_frames.frames.empty() ? 0 : effective_frames.frames[0].width) +
          "x" +
          std::to_string(effective_frames.frames.empty() ? 0 : effective_frames.frames[0].height) +
          " resolution; collected " +
          std::to_string(all_detections.size()) + " per-frame detection(s)");
  detector_proc["diagnostics"] = frame_diagnostics;
  result.processors.push_back(detector_proc);

  nlohmann::json recognizer_proc = make_ocr_processor_provenance(
      "processor_ocr_recognizer_0001", "ocr_recognizer",
      "svp-vision-ocr-generation-v1", "tesseract_subprocess",
      "completed",
      "Text recognition via tesseract subprocess with multi-frame reconciliation; "
      "produced " +
          std::to_string(result.text_observation_count) +
          " reconciled text observation(s) from " +
          std::to_string(all_detections.size()) + " per-frame detection(s)");
  recognizer_proc["diagnostics"] = frame_diagnostics;
  result.processors.push_back(recognizer_proc);

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
      {"blocker", sanitize_utf8(result.blocker)},
      {"text_regions", regions_arr},
      {"text_observations", obs_arr},
      {"numeric_values", num_arr},
      {"text_absence", text_absence_to_json(result.text_absence)},
      {"processors", result.processors},
  };
}

}  // namespace svp::vision
