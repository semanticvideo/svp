#include "svp/vision/ocr_generation.hpp"

#include "svp/media/media_ingest_plan.hpp"
#include "svp/vision/evidence_crop.hpp"
#include "svp/vision/foundation_ocr_staging.hpp"
#include "svp/vision/pp_ocr.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace svp::vision {
namespace {

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

std::string trim(const std::string& s) {
  std::size_t start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  std::size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

std::string lower(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return out;
}

std::string normalize_text(const std::string& raw) {
  std::string result = trim(raw);
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

// --- Numeric parsing ---

struct ParsedNumber {
  std::string raw_text;
  std::string normalized_text;
  std::string number_kind;
  std::string numeric_value;
  std::string unit;
  double confidence;
};

bool looks_like_date_context(const std::string& text) {
  static const std::regex date_re(
      R"((january|february|march|april|may|june|july|august|september|october|november|december)\s+\d{1,2}(?:,\s*|\s+)\d{2,4})",
      std::regex_constants::icase);
  return std::regex_search(text, date_re);
}

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
  static const std::regex currency_re(R"(\$(\d+)(?:\.\s*(\d+))?)");
  static const std::regex decimal_re(R"(\b(\d+\.\d+)\b)");
  static const std::regex integer_re(R"(\b(\d+)\b)");

  std::string::const_iterator search_start = raw_text.cbegin();
  std::smatch match;
  std::vector<std::pair<std::size_t, std::size_t>> matched_ranges;

  search_start = raw_text.cbegin();
  while (std::regex_search(search_start, raw_text.cend(), match, currency_re)) {
    ParsedNumber num;
    num.raw_text = match[0].str();
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

  search_start = raw_text.cbegin();
  while (std::regex_search(search_start, raw_text.cend(), match, decimal_re)) {
    std::size_t pos = match[0].first - raw_text.cbegin();
    std::size_t end_pos = pos + match[0].length();
    bool overlaps = false;
    for (const auto& r : matched_ranges) {
      if (pos < r.second && end_pos > r.first) { overlaps = true; break; }
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

  const bool is_date = looks_like_date_context(raw_text);
  if (is_date) {
    search_start = raw_text.cbegin();
    while (std::regex_search(search_start, raw_text.cend(), match, integer_re)) {
      std::size_t pos = match[0].first - raw_text.cbegin();
      std::size_t end_pos = pos + match[0].length();
      bool overlaps = false;
      for (const auto& r : matched_ranges) {
        if (pos < r.second && end_pos > r.first) { overlaps = true; break; }
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

// --- Multi-frame reconciliation ---

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

std::vector<ReconciledObservation> reconcile_detections(
    const std::vector<FrameDetection>& detections,
    int total_frames,
    double min_confidence = 0.30,
    std::size_t min_text_chars = 3) {
  std::vector<ReconciledObservation> reconciled;

  std::map<std::string, std::vector<std::size_t>> by_text_key;
  for (std::size_t i = 0; i < detections.size(); ++i) {
    const std::string norm = normalize_text(detections[i].raw_text);
    const std::string key = alphanumeric_key(norm);
    if (key.length() < min_text_chars) continue;
    by_text_key[key].push_back(i);
  }

  for (const auto& [key, indices] : by_text_key) {
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

      if (total_frames > 1 && cluster.size() == 1 && obs.confidence < min_confidence) {
        continue;
      }

      if (cluster.size() > 1) {
        obs.confidence = std::min(1.0, obs.confidence + 0.1 * (cluster.size() - 1));
      }

      reconciled.push_back(std::move(obs));
    }
  }

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
    std::ofstream out(text_dir / "evidence_crops.jsonl");
  }
  {
    std::ofstream out(text_dir / "text_absence.json");
    if (out) {
      out << text_absence_to_json(text_absence).dump(2) << "\n";
    }
  }
}

}  // namespace

OcrSourceFrameDimensions derive_ocr_source_frame_dimensions(
    int stored_width,
    int stored_height,
    int rotation_degrees) {
  const int normalized_rotation = ((rotation_degrees % 360) + 360) % 360;
  const bool swaps_axes =
      normalized_rotation == 90 || normalized_rotation == 270;
  return swaps_axes
      ? OcrSourceFrameDimensions{stored_height, stored_width}
      : OcrSourceFrameDimensions{stored_width, stored_height};
}

OcrGenerationResult generate_ocr_observations(
    const OcrGenerationOptions& options,
    const DecodedCanonicalFrames& frame_input,
    const std::filesystem::path& staging_dir) {
  OcrGenerationResult result;

  // Create PP-OCR session
  PpOcrOptions pp_ocr_opts;
  pp_ocr_opts.model_cache_root = options.model_cache_root;
  pp_ocr_opts.execution_provider = "cpu";

  PpOcrSession pp_ocr_session = create_pp_ocr_session(pp_ocr_opts);

  result.ocr_available = pp_ocr_session.available;
  result.ocr_frame_input_available =
      frame_input.decoding_succeeded && !frame_input.frames.empty();

  // Decode higher-resolution frames for OCR if media_plan is provided
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

  const DecodedCanonicalFrames& effective_frames =
      using_high_res_frames ? ocr_frames : frame_input;

  // If PP-OCR is not available, report honest blocker
  if (!result.ocr_available) {
    result.blocker = pp_ocr_session.blocker;
    result.text_absence.schema_version = "svp-text-absence-v1";
    result.text_absence.ocr_required = true;
    result.text_absence.ocr_completed = false;
    result.text_absence.reason = "processor_failed";
    result.text_absence.provenance_id = "processor_ocr_detector_0001";
    result.processors.push_back(make_ocr_processor_provenance(
        "processor_ocr_detector_0001", "ocr_detector",
        "svp-vision-pp-ocr-v1", "onnxruntime",
        "not_executed", result.blocker));
    result.processors.push_back(make_ocr_processor_provenance(
        "processor_ocr_recognizer_0001", "ocr_recognizer",
        "svp-vision-pp-ocr-v1", "onnxruntime",
        "not_executed", "PP-OCR not available"));
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

  // If frames are not available, report blocker
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
        "svp-vision-pp-ocr-v1", "onnxruntime",
        "not_executed", result.blocker));
    result.processors.push_back(make_ocr_processor_provenance(
        "processor_ocr_recognizer_0001", "ocr_recognizer",
        "svp-vision-pp-ocr-v1", "onnxruntime",
        "not_executed", result.blocker));
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

  // Phase 1: Run PP-OCR on each frame
  std::vector<FrameDetection> all_detections;
  std::vector<nlohmann::json> frame_diagnostics;
  bool any_frame_failed = false;
  std::string failure_reason_details;

  for (std::size_t frame_idx = 0; frame_idx < effective_frames.frames.size(); ++frame_idx) {
    const ColorRasterFrame& frame = effective_frames.frames[frame_idx];

    if (frame.width <= 0 || frame.height <= 0 || frame.pixels.empty()) {
      nlohmann::json diag = {
          {"frame_id", sanitize_utf8(frame.frame_id)},
          {"timestamp_us", frame.timestamp_us},
          {"extraction_succeeded", false},
          {"extraction_error", "Empty or invalid frame pixels"},
          {"pp_ocr_attempted", false}
      };
      frame_diagnostics.push_back(diag);
      any_frame_failed = true;
      if (failure_reason_details.empty()) {
        failure_reason_details = sanitize_utf8("Invalid frame data for " + frame.frame_id);
      }
      continue;
    }

    PpOcrFrameResult ocr_result;
    try {
      ocr_result = run_pp_ocr_on_frame(pp_ocr_session, pp_ocr_opts, frame);
    } catch (const std::exception& e) {
      nlohmann::json diag = {
          {"frame_id", sanitize_utf8(frame.frame_id)},
          {"timestamp_us", frame.timestamp_us},
          {"extraction_succeeded", true},
          {"pp_ocr_attempted", true},
          {"pp_ocr_succeeded", false},
          {"pp_ocr_error", sanitize_utf8(e.what())}
      };
      frame_diagnostics.push_back(diag);
      any_frame_failed = true;
      if (failure_reason_details.empty()) {
        failure_reason_details = sanitize_utf8("PP-OCR failed on frame " + frame.frame_id + ": " + e.what());
      }
      continue;
    }

    nlohmann::json diag = {
        {"frame_id", sanitize_utf8(frame.frame_id)},
        {"timestamp_us", frame.timestamp_us},
        {"extraction_succeeded", true},
        {"pp_ocr_attempted", true},
        {"pp_ocr_succeeded", true},
        {"detection_count", ocr_result.detections.size()}
    };
    frame_diagnostics.push_back(diag);

    if (ocr_result.detections.empty()) continue;

    for (const auto& det : ocr_result.detections) {
      if (det.bbox_right <= det.bbox_left || det.bbox_bottom <= det.bbox_top) continue;

      FrameDetection fdet;
      fdet.frame_id = frame.frame_id;
      fdet.timestamp_us = frame.timestamp_us;
      fdet.frame_index = frame_idx;
      fdet.frame_width = frame.width;
      fdet.frame_height = frame.height;
      fdet.raw_text = det.text;
      fdet.confidence = det.score;
      fdet.bbox_left = det.bbox_left;
      fdet.bbox_top = det.bbox_top;
      fdet.bbox_right = det.bbox_right;
      fdet.bbox_bottom = det.bbox_bottom;
      all_detections.push_back(std::move(fdet));
    }
  }

  // Surface any failure as a blocker
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
        "svp-vision-pp-ocr-v1", "onnxruntime",
        "failed",
        "Text detection failed: " + failure_reason_details);
    detector_proc["diagnostics"] = frame_diagnostics;
    result.processors.push_back(detector_proc);

    nlohmann::json recognizer_proc = make_ocr_processor_provenance(
        "processor_ocr_recognizer_0001", "ocr_recognizer",
        "svp-vision-pp-ocr-v1", "onnxruntime",
        "failed",
        "Text recognition failed: " + failure_reason_details);
    recognizer_proc["diagnostics"] = frame_diagnostics;
    result.processors.push_back(recognizer_proc);

    result.processors.push_back(make_ocr_processor_provenance(
        "processor_numeric_parser_0001", "numeric_parser",
        "svp-vision-ocr-generation-v1", "deterministic_cpp",
        "not_run", "No OCR text to parse due to OCR execution failure"));

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
      std::ofstream out(text_dir / "evidence_crops.jsonl");
    }
    result.evidence_crops_written = true;
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

  // Phase 4: Generate evidence crops
  if (options.generate_evidence_crops &&
      options.media_plan != nullptr &&
      !result.text_observations.empty()) {
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
    crop_opts.crop_image_format = "jpeg";
    crop_opts.jpeg_quality = 95;

    EvidenceCropResult crop_result;
    try {
      crop_result = generate_evidence_crops_internal(
          crop_opts, crop_inputs, staging_dir);
    } catch (const std::exception& e) {
      crop_result.crops_written = false;
      crop_result.crops_skipped_reason =
          std::string("Evidence crop generation error: ") + e.what();
    }

    result.evidence_crops = crop_result.crops;
    result.evidence_crop_count = crop_result.crop_count;
    result.evidence_crop_total_bytes = crop_result.total_crop_bytes;
    result.evidence_crops_written = crop_result.crops_written;
    result.evidence_crops_skipped = crop_result.crops_skipped_count;
    result.evidence_crops_skipped_reason = crop_result.crops_skipped_reason;
    result.roi_hardening_run = true;

    std::unordered_map<std::string, std::string> obs_id_to_crop_id;
    for (const auto& crop : crop_result.crops) {
      obs_id_to_crop_id[crop.text_observation_id] = crop.crop_id;
    }
    for (auto& obs : result.text_observations) {
      auto it = obs_id_to_crop_id.find(obs.text_observation_id);
      if (it != obs_id_to_crop_id.end()) {
        obs.evidence_crop_refs.push_back(it->second);
      }
    }

    // Apply ROI hardening results
    std::unordered_map<std::string, std::size_t> obs_id_to_roi_idx;
    for (std::size_t i = 0; i < crop_inputs.size(); ++i) {
      obs_id_to_roi_idx[crop_inputs[i].text_observation_id] = i;
    }
    for (auto& obs : result.text_observations) {
      auto it = obs_id_to_roi_idx.find(obs.text_observation_id);
      if (it == obs_id_to_roi_idx.end()) continue;
      const auto& roi = crop_result.roi_ocr_results[it->second];
      if (!roi.succeeded) continue;

      const int current_words = count_words(obs.raw_text);
      if (roi.word_count > current_words) {
        obs.raw_text = roi.raw_text;
        obs.normalized_text = normalize_text(roi.raw_text);
        obs.confidence = roi.confidence;
      }
    }
  }

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

  // Build processor provenance with PP-OCR model info
  nlohmann::json detector_model_refs = nlohmann::json::array();
  detector_model_refs.push_back(pp_ocr_model_info_to_json(pp_ocr_session.model_info));

  nlohmann::json detector_proc = make_ocr_processor_provenance(
      "processor_ocr_detector_0001", "ocr_detector",
      "svp-vision-pp-ocr-v1", "onnxruntime",
      "completed",
      "Text detection via PP-OCR ONNX (DB post-processing) on " +
          std::to_string(effective_frames.frames.size()) +
          " decoded frame(s) at " +
          std::to_string(effective_frames.frames.empty() ? 0 : effective_frames.frames[0].width) +
          "x" +
          std::to_string(effective_frames.frames.empty() ? 0 : effective_frames.frames[0].height) +
          " resolution; collected " +
          std::to_string(all_detections.size()) + " per-frame detection(s)");
  detector_proc["model_refs"] = detector_model_refs;
  detector_proc["diagnostics"] = frame_diagnostics;
  result.processors.push_back(detector_proc);

  nlohmann::json recognizer_proc = make_ocr_processor_provenance(
      "processor_ocr_recognizer_0001", "ocr_recognizer",
      "svp-vision-pp-ocr-v1", "onnxruntime",
      "completed",
      "Text recognition via PP-OCR ONNX (CTC decode) with multi-frame reconciliation; "
      "produced " +
          std::to_string(result.text_observation_count) +
          " reconciled text observation(s) from " +
          std::to_string(all_detections.size()) + " per-frame detection(s)");
  recognizer_proc["model_refs"] = detector_model_refs;
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

  if (!result.evidence_crops_written) {
    std::ofstream out(text_dir / "evidence_crops.jsonl");
    result.evidence_crops_written = true;
  }

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
      {"evidence_crops_written", result.evidence_crops_written},
      {"evidence_crop_count", result.evidence_crop_count},
      {"evidence_crop_total_bytes", result.evidence_crop_total_bytes},
      {"evidence_crops_skipped", result.evidence_crops_skipped},
      {"evidence_crops_skipped_reason", sanitize_utf8(result.evidence_crops_skipped_reason)},
      {"roi_hardening_run", result.roi_hardening_run},
      {"evidence_crops", evidence_crop_result_to_json(
          EvidenceCropResult{
              result.evidence_crops,
              result.evidence_crop_total_bytes,
              result.evidence_crops_written,
              result.evidence_crop_count,
              result.evidence_crops_skipped,
              result.evidence_crops_skipped_reason,
              {}})},
  };
}

}  // namespace svp::vision
