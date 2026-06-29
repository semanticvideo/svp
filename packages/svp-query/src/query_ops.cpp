#include "svp/query/query_ops.hpp"

#include "svp/query/query_reader.hpp"
#include "svp/package/package_layout.hpp"
#include "svp/package/relationship_type_policy.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace svp::query {
namespace {

struct KnownLayer {
  std::string_view section;
  std::string_view entry;
  std::string_view kind;
};

constexpr std::array<KnownLayer, 22> kKnownLayers{{
    {"core", "manifest.json", "json"},
    {"core", "mimetype", "text"},
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

}  // namespace svp::query
