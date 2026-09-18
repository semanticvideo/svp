#include "svp/query/query_ops.hpp"

#include "svp/query/query_reader.hpp"
#include "svp/package/package_layout.hpp"
#include "svp/package/relationship_type_policy.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_map>

namespace svp::query {
namespace {

struct KnownLayer {
  std::string_view section;
  std::string_view entry;
  std::string_view kind;
};

constexpr std::array<KnownLayer, 28> kKnownLayers{{
    {"core", "manifest.json", "json"},
    {"core", "mimetype", "text"},
    {"media", "media/audio/audio_absence.json", "json"},
    {"media", "media/audio/waveform.jsonl", "jsonl"},
    {"media", "media/audio/loudness.jsonl", "jsonl"},
    {"media", "media/audio/loudness_summary.json", "json"},
    {"media", "media/audio/spectrum.jsonl", "jsonl"},
    {"media", "media/audio/spectrum_summary.json", "json"},
    {"transcript", "transcript/transcript.json", "json"},
    {"transcript", "transcript/words.jsonl", "jsonl"},
    {"transcript", "transcript/speakers.jsonl", "jsonl"},
    {"transcript", "transcript/speaker_segments.jsonl", "jsonl"},
    {"transcript", "transcript/speech_regions.jsonl", "jsonl"},
    {"timeline", "timeline/frames.jsonl", "jsonl"},
    {"timeline", "timeline/shots.jsonl", "jsonl"},
    {"timeline", "timeline/scenes.jsonl", "jsonl"},
    {"entities", "entities/entities.jsonl", "jsonl"},
    {"entities", "entities/entity_tracks.jsonl", "jsonl"},
    {"text", "text/text_regions.jsonl", "jsonl"},
    {"text", "text/text_observations.jsonl", "jsonl"},
    {"text", "text/numeric_values.jsonl", "jsonl"},
    {"text", "text/text_absence.json", "json"},
    {"text", "text/evidence_crops.jsonl", "jsonl"},
    {"colors", "colors/color_observations.jsonl", "jsonl"},
    {"colors", "colors/color_summary.json", "json"},
    {"colors", "colors/color_absence.json", "json"},
    {"index", "index/index_manifest.json", "json"},
    {"provenance", "provenance/validation.json", "json"},
}};

std::string to_lower(std::string_view s) {
  std::string result{s};
  std::ranges::transform(result, result.begin(),
                          [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return result;
}

bool contains_ci(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) {
    return true;
  }
  const auto hay = to_lower(haystack);
  const auto ndl = to_lower(needle);
  return hay.find(ndl) != std::string::npos;
}

std::string json_string(const nlohmann::json& j, std::string_view key) {
  if (!j.is_object()) {
    return {};
  }
  const auto it = j.find(key);
  if (it == j.end() || !it->is_string()) {
    return {};
  }
  return it->get<std::string>();
}

}  // namespace

PackageLayerSummary list_layers(const std::filesystem::path& package_path) {
  PackageLayerSummary result;

  for (const auto& layer : kKnownLayers) {
    LayerInfo info;
    info.section = std::string{layer.section};
    info.entry = std::string{layer.entry};
    info.kind = std::string{layer.kind};

    if (info.kind == "jsonl") {
      const auto jsonl = read_jsonl_entry(package_path, info.entry);
      info.present = jsonl.present;
      if (jsonl.readable) {
        info.record_count = jsonl.records.size();
        info.has_malformed = jsonl.has_malformed;
        info.malformed_line_count = jsonl.malformed_line_count;
        info.error_message = jsonl.error_message;
      }
    } else if (info.kind == "text") {
      const auto layout = svp::package::read_package_layout(package_path);
      if (layout.has_value()) {
        info.present = layout.value().has_entry(info.entry);
        info.record_count = info.present ? 1 : 0;
      }
    } else {
      const auto json = read_json_entry(package_path, info.entry);
      info.present = json.present;
      info.record_count = json.parsed ? 1 : 0;
      if (json.present && !json.parsed) {
        info.has_malformed = true;
        info.error_message = json.error_message;
      }
    }

    result.layers.push_back(std::move(info));
  }

  result.total_entries =
      static_cast<std::uint64_t>(std::ranges::count_if(
          result.layers, [](const LayerInfo& l) { return l.present; }));

  return result;
}

TranscriptSummaryResult transcript_summary(const std::filesystem::path& package_path) {
  TranscriptSummaryResult result;

  const auto transcript = read_json_entry(package_path, "transcript/transcript.json");
  if (!transcript.present) {
    return result;
  }

  result.present = true;
  if (!transcript.parsed) {
    result.error_message = transcript.error_message;
    return result;
  }

  result.parsed = true;
  result.transcript_json = transcript.value;

  const auto words = read_jsonl_entry(package_path, "transcript/words.jsonl");
  if (words.readable) {
    result.word_count_file = words.records.size();
  }

  const auto speakers = read_jsonl_entry(package_path, "transcript/speakers.jsonl");
  if (speakers.readable) {
    result.speaker_count_file = speakers.records.size();
  }

  const auto speech_regions = read_jsonl_entry(package_path, "transcript/speech_regions.jsonl");
  if (speech_regions.readable) {
    result.speech_region_count = speech_regions.records.size();
  }

  return result;
}

std::vector<WordMatch> find_words(const std::filesystem::path& package_path,
                                  const std::string& search_text,
                                  std::size_t max_results) {
  std::vector<WordMatch> matches;

  const auto words = read_jsonl_entry(package_path, "transcript/words.jsonl");
  if (!words.readable) {
    return matches;
  }

  for (const auto& record : words.records) {
    const auto text = json_string(record, "text");
    const auto normalized = json_string(record, "normalized_text");
    if (contains_ci(text, search_text) || contains_ci(normalized, search_text)) {
      matches.push_back({record});
      if (matches.size() >= max_results) {
        break;
      }
    }
  }

  return matches;
}

std::vector<SpeakerInfo> list_speakers(const std::filesystem::path& package_path) {
  std::vector<SpeakerInfo> speakers;

  const auto speakers_jsonl = read_jsonl_entry(package_path, "transcript/speakers.jsonl");
  if (!speakers_jsonl.readable) {
    return speakers;
  }

  std::vector<std::pair<std::string, std::uint64_t>> word_counts;

  const auto words = read_jsonl_entry(package_path, "transcript/words.jsonl");
  if (words.readable) {
    for (const auto& word : words.records) {
      const auto speaker_id = json_string(word, "speaker_id");
      if (!speaker_id.empty()) {
        auto it = std::ranges::find(word_counts, speaker_id,
                                     &std::pair<std::string, std::uint64_t>::first);
        if (it != word_counts.end()) {
          ++it->second;
        } else {
          word_counts.emplace_back(speaker_id, 1);
        }
      }
    }
  }

  for (const auto& speaker_record : speakers_jsonl.records) {
    SpeakerInfo info;
    info.record = speaker_record;
    const auto sid = json_string(speaker_record, "id");
    auto it = std::ranges::find(word_counts, sid,
                                 &std::pair<std::string, std::uint64_t>::first);
    if (it != word_counts.end()) {
      info.word_count = it->second;
    }
    speakers.push_back(std::move(info));
  }

  return speakers;
}

std::vector<OcrObservationInfo> list_ocr_observations(
    const std::filesystem::path& package_path,
    const std::optional<std::string>& text_filter,
    std::size_t max_results) {
  std::vector<OcrObservationInfo> observations;

  const auto obs = read_jsonl_entry(package_path, "text/text_observations.jsonl");
  if (!obs.readable) {
    return observations;
  }

  for (const auto& record : obs.records) {
    if (text_filter.has_value()) {
      const auto raw_text = json_string(record, "raw_text");
      const auto normalized = json_string(record, "normalized_text");
      if (!contains_ci(raw_text, *text_filter) && !contains_ci(normalized, *text_filter)) {
        continue;
      }
    }
    observations.push_back({record});
    if (observations.size() >= max_results) {
      break;
    }
  }

  return observations;
}

std::vector<ColorObservationInfo> list_color_observations(
    const std::filesystem::path& package_path,
    const std::optional<std::string>& dominant_bucket_filter,
    std::optional<double> min_coverage_threshold,
    std::size_t max_results) {
  std::vector<ColorObservationInfo> observations;

  const auto obs = read_jsonl_entry(package_path, "colors/color_observations.jsonl");
  if (!obs.readable) {
    return observations;
  }

  for (const auto& record : obs.records) {
    if (dominant_bucket_filter.has_value()) {
      const auto dominant = json_string(record, "dominant_bucket");
      if (!contains_ci(dominant, *dominant_bucket_filter)) {
        continue;
      }
    }

    if (min_coverage_threshold.has_value()) {
      const auto dom_bucket = json_string(record, "dominant_bucket");
      if (!dom_bucket.empty()) {
        const auto buckets = record.find("bucket_coverage");
        if (buckets != record.end() && buckets->is_object()) {
          const auto bucket_it = buckets->find(dom_bucket);
          if (bucket_it != buckets->end() && bucket_it->is_number()) {
            const auto coverage = bucket_it->get<double>();
            if (coverage < *min_coverage_threshold) {
              continue;
            }
          }
        }
      }
    }

    observations.push_back({record});
    if (observations.size() >= max_results) {
      break;
    }
  }

  return observations;
}

ValidationInfo show_validation(const std::filesystem::path& package_path) {
  ValidationInfo info;

  const auto validation = read_json_entry(package_path, "provenance/validation.json");
  info.present = validation.present;
  if (!validation.parsed) {
    info.error_message = validation.error_message;
    return info;
  }

  info.parsed = true;
  info.record = validation.value;
  return info;
}

std::vector<RelationshipInfo> list_relationships(
    const std::filesystem::path& package_path,
    const std::optional<std::string>& class_filter,
    std::size_t max_results) {
  std::vector<RelationshipInfo> results;

  const auto jsonl = read_jsonl_entry(package_path, "relationships/relationships.jsonl");
  if (!jsonl.readable) {
    return results;
  }

  for (const auto& record : jsonl.records) {
    const auto type = json_string(record, "type");
    const auto cls = svp::package::classify_relationship_type(type);
    const auto class_str = std::string{svp::package::relationship_class_to_string(cls)};

    if (class_filter.has_value() && *class_filter != class_str) {
      continue;
    }

    RelationshipInfo info;
    info.record = record;
    info.relationship_class = class_str;
    results.push_back(std::move(info));
    if (results.size() >= max_results) {
      break;
    }
  }

  return results;
}

RelationshipSummary relationship_summary(const std::filesystem::path& package_path) {
  RelationshipSummary summary;

  const auto jsonl = read_jsonl_entry(package_path, "relationships/relationships.jsonl");
  summary.present = jsonl.present;
  if (!jsonl.readable) {
    summary.error_message = jsonl.error_message;
    return summary;
  }

  summary.readable = true;

  for (const auto& record : jsonl.records) {
    const auto type = json_string(record, "type");
    const auto cls = svp::package::classify_relationship_type(type);

    ++summary.total_count;
    switch (cls) {
      case svp::package::RelationshipClass::Support:
        ++summary.support_count;
        break;
      case svp::package::RelationshipClass::Semantic:
        ++summary.semantic_count;
        break;
      case svp::package::RelationshipClass::Unknown:
        ++summary.unknown_count;
        break;
    }
  }

  return summary;
}

LoudnessRangeResult loudness_range(const std::filesystem::path& package_path,
                                   std::int64_t start_us,
                                   std::int64_t end_us,
                                   const std::optional<std::string>& target_id) {
  // BS.1770 gating constants: the absolute gate is -70 LUFS and the relative
  // gate sits 10 LU below the ungated mean. Stored momentary values already
  // carry the standard's channel-weight normalization, so energy derived from
  // them needs no further offset.
  constexpr double kAbsoluteGateLufs = -70.0;
  constexpr double kRelativeGateOffsetLu = 10.0;

  LoudnessRangeResult result;
  result.start_us = start_us;
  result.end_us = end_us;

  const auto jsonl = read_jsonl_entry(package_path, "media/audio/loudness.jsonl");
  result.present = jsonl.present;
  if (!jsonl.readable) {
    result.error_message = jsonl.error_message;
    return result;
  }
  result.readable = true;
  if (end_us <= start_us) {
    result.error_message = "end_us must be greater than start_us";
    return result;
  }

  struct Window {
    double momentary_lufs;
    double energy;
    std::int64_t overlap_us;
  };
  std::unordered_map<std::string, std::vector<Window>> windows_per_stream;
  std::unordered_map<std::string, double> max_peak_linear_per_stream;
  std::unordered_map<std::string, std::int64_t> covered_us_per_stream;
  std::unordered_map<std::string, std::uint64_t> window_count_per_stream;

  for (const auto& record : jsonl.records) {
    const std::string record_target = json_string(record, "target_id");
    if (target_id.has_value() && record_target != *target_id) {
      continue;
    }
    const auto start_it = record.find("start_us");
    const auto end_it = record.find("end_us");
    if (start_it == record.end() || end_it == record.end() ||
        !start_it->is_number() || !end_it->is_number()) {
      continue;
    }
    const std::int64_t record_start = start_it->get<std::int64_t>();
    const std::int64_t record_end = end_it->get<std::int64_t>();
    if (record_end <= record_start || record_end <= start_us ||
        record_start >= end_us) {
      continue;
    }

    ++window_count_per_stream[record_target];
    const std::int64_t overlap_us =
        std::min(record_end, end_us) - std::max(record_start, start_us);
    covered_us_per_stream[record_target] += overlap_us;

    const auto momentary_it = record.find("momentary_lufs");
    if (momentary_it != record.end() && momentary_it->is_number()) {
      const double momentary = momentary_it->get<double>();
      windows_per_stream[record_target].push_back(
          Window{momentary, std::pow(10.0, momentary / 10.0), overlap_us});
    }

    const auto peak_it = record.find("true_peak_dbtp");
    if (peak_it != record.end() && peak_it->is_number()) {
      const double linear = std::pow(10.0, peak_it->get<double>() / 20.0);
      max_peak_linear_per_stream[record_target] =
          std::max(max_peak_linear_per_stream[record_target], linear);
    }
  }

  std::vector<std::string> stream_ids;
  for (const auto& [id, windows] : windows_per_stream) {
    stream_ids.push_back(id);
  }
  for (const auto& [id, count] : window_count_per_stream) {
    if (windows_per_stream.find(id) == windows_per_stream.end()) {
      stream_ids.push_back(id);
    }
  }
  std::ranges::sort(stream_ids);
  stream_ids.erase(std::unique(stream_ids.begin(), stream_ids.end()),
                   stream_ids.end());

  for (const std::string& id : stream_ids) {
    LoudnessRangeStreamResult stream_result;
    stream_result.target_id = id;
    stream_result.window_count = window_count_per_stream[id];
    stream_result.covered_us = covered_us_per_stream[id];

    std::vector<Window> gated;
    for (const Window& window : windows_per_stream[id]) {
      if (window.momentary_lufs > kAbsoluteGateLufs) {
        gated.push_back(window);
      }
    }

    if (!gated.empty()) {
      // Each window contributes energy in proportion to how much of it the
      // queried range actually covers; normalization divides by total covered
      // duration so a partially overlapped window does not count fully.
      double weighted_energy = 0.0;
      double weighted_us = 0.0;
      for (const Window& window : gated) {
        weighted_energy += window.energy * static_cast<double>(window.overlap_us);
        weighted_us += static_cast<double>(window.overlap_us);
      }
      const double ungated_lufs =
          10.0 * std::log10(weighted_energy / weighted_us);
      const double relative_gate = ungated_lufs - kRelativeGateOffsetLu;

      double gated_energy = 0.0;
      double gated_us = 0.0;
      for (const Window& window : gated) {
        if (window.momentary_lufs > relative_gate) {
          gated_energy += window.energy * static_cast<double>(window.overlap_us);
          gated_us += static_cast<double>(window.overlap_us);
        }
      }
      if (gated_us > 0.0) {
        stream_result.integrated_lufs =
            std::round(10.0 * std::log10(gated_energy / gated_us) * 10.0) /
            10.0;
      }
    }

    const double max_peak = max_peak_linear_per_stream[id];
    if (max_peak > 0.0) {
      stream_result.true_peak_dbtp =
          std::round(20.0 * std::log10(max_peak) * 10.0) / 10.0;
    }

    result.streams.push_back(std::move(stream_result));
  }

  return result;
}

LoudnessSummaryInfo loudness_summary(const std::filesystem::path& package_path) {
  LoudnessSummaryInfo info;
  const auto json = read_json_entry(package_path, "media/audio/loudness_summary.json");
  info.present = json.present;
  if (!json.present || !json.parsed) {
    info.error_message = json.error_message;
    return info;
  }
  info.parsed = true;
  info.record = json.value;
  return info;
}

SpectrumRangeResult spectrum_range(const std::filesystem::path& package_path,
                                   std::int64_t start_us,
                                   std::int64_t end_us,
                                   const std::optional<std::string>& target_id) {
  SpectrumRangeResult result;
  result.start_us = start_us;
  result.end_us = end_us;

  const auto jsonl = read_jsonl_entry(package_path, "media/audio/spectrum.jsonl");
  result.present = jsonl.present;
  if (!jsonl.readable) {
    result.error_message = jsonl.error_message;
    return result;
  }
  result.readable = true;
  if (end_us <= start_us) {
    result.error_message = "end_us must be greater than start_us";
    return result;
  }

  struct BandAccumulator {
    double power_sum = 0.0;
    double power_max = 0.0;
    std::int64_t measured_us = 0;
  };

  std::unordered_map<std::string, std::array<BandAccumulator, kSpectrumBandCount>>
      bands_per_stream;
  std::unordered_map<std::string, std::int64_t> covered_us_per_stream;
  std::unordered_map<std::string, std::uint64_t> window_count_per_stream;

  for (const auto& record : jsonl.records) {
    const std::string record_target = json_string(record, "target_id");
    if (target_id.has_value() && record_target != *target_id) {
      continue;
    }
    const auto start_it = record.find("start_us");
    const auto end_it = record.find("end_us");
    const auto bands_it = record.find("bands");
    if (start_it == record.end() || end_it == record.end() ||
        bands_it == record.end() || !start_it->is_number() ||
        !end_it->is_number() || !bands_it->is_array()) {
      continue;
    }
    const std::int64_t record_start = start_it->get<std::int64_t>();
    const std::int64_t record_end = end_it->get<std::int64_t>();
    if (record_end <= record_start || record_end <= start_us ||
        record_start >= end_us) {
      continue;
    }

    ++window_count_per_stream[record_target];
    const std::int64_t overlap_us =
        std::min(record_end, end_us) - std::max(record_start, start_us);
    covered_us_per_stream[record_target] += overlap_us;

    auto& accumulators = bands_per_stream[record_target];
    for (std::size_t band = 0;
         band < bands_it->size() && band < kSpectrumBandCount; ++band) {
      const nlohmann::json& value = (*bands_it)[band];
      if (!value.is_number()) {
        continue;
      }
      // Weight each band's energy by how much of the window the range covers.
      const double power = std::pow(10.0, value.get<double>() / 10.0);
      accumulators[band].power_sum +=
          power * static_cast<double>(overlap_us);
      accumulators[band].power_max =
          std::max(accumulators[band].power_max, power);
      accumulators[band].measured_us += overlap_us;
    }
  }

  std::vector<std::string> stream_ids;
  for (const auto& [id, count] : window_count_per_stream) {
    stream_ids.push_back(id);
  }
  std::ranges::sort(stream_ids);

  for (const std::string& id : stream_ids) {
    SpectrumRangeStreamResult stream_result;
    stream_result.target_id = id;
    stream_result.window_count = window_count_per_stream[id];
    stream_result.covered_us = covered_us_per_stream[id];

    const auto& accumulators = bands_per_stream[id];
    for (std::size_t band = 0; band < kSpectrumBandCount; ++band) {
      const BandAccumulator& acc = accumulators[band];
      if (acc.measured_us > 0 && acc.power_sum > 0.0) {
        stream_result.mean_band_dbfs[band] =
            std::round(10.0 * std::log10(acc.power_sum /
                                         static_cast<double>(acc.measured_us)) *
                       10.0) /
            10.0;
      }
      if (acc.power_max > 0.0) {
        stream_result.max_band_dbfs[band] =
            std::round(10.0 * std::log10(acc.power_max) * 10.0) / 10.0;
      }
    }

    result.streams.push_back(std::move(stream_result));
  }

  return result;
}

SpectrumSummaryInfo spectrum_summary(const std::filesystem::path& package_path) {
  SpectrumSummaryInfo info;
  const auto json = read_json_entry(package_path, "media/audio/spectrum_summary.json");
  info.present = json.present;
  if (!json.present || !json.parsed) {
    info.error_message = json.error_message;
    return info;
  }
  info.parsed = true;
  info.record = json.value;
  return info;
}

}  // namespace svp::query
