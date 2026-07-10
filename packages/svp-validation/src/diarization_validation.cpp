#include "diarization_validation.hpp"

#include "svp/package/package_layout.hpp"

#include <nlohmann/json.hpp>

#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace svp::validation {
namespace {

bool has_entry(const svp::package::PackageLayout& layout, const std::string& entry) {
  return layout.has_entry(entry);
}

void add_microphone_provenance_error(
    ValidationReport& report, const ValidationCodeRegistry& registry,
    const std::string& path, const std::string& message) {
  add_finding(report, make_finding(registry, kCodeDiarizationUnavailable,
                                   path, message));
}

std::vector<nlohmann::json> read_jsonl_entry(
    const std::filesystem::path& package_path,
    const svp::package::PackageLayout& layout,
    const std::string& entry) {
  if (!layout.has_entry(entry)) return {};
  const auto read_result = svp::package::read_package_entry(package_path, entry);
  if (!read_result.has_value()) return {};
  std::vector<nlohmann::json> records;
  std::istringstream input(read_result.value());
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty()) records.push_back(nlohmann::json::parse(line));
  }
  return records;
}

void validate_microphone_provenance(
    ValidationReport& report, const ValidationCodeRegistry& registry,
    const std::filesystem::path& package_path,
    const svp::package::PackageLayout& layout,
    const nlohmann::json& transcript, std::size_t speaker_count) {
  const auto& diarization = transcript["diarization"];
  if (!diarization.contains("processor_id") ||
      !diarization["processor_id"].is_string() ||
      diarization["processor_id"].get<std::string>().empty()) {
    add_microphone_provenance_error(
        report, registry, "/transcript/transcript.json/diarization/processor_id",
        "Microphone stream assignment requires a processor_id.");
    return;
  }
  const std::string processor_id =
      diarization["processor_id"].get<std::string>();

  if (!transcript.contains("speaker_sources") ||
      !transcript["speaker_sources"].is_array() ||
      transcript["speaker_sources"].size() != speaker_count) {
    add_microphone_provenance_error(
        report, registry, "/transcript/transcript.json/speaker_sources",
        "Microphone speaker_sources must contain one mapping per speaker.");
  } else {
    std::set<std::string> speaker_ids;
    for (const auto& source : transcript["speaker_sources"]) {
      if (!source.is_object() || !source.contains("speaker_id") ||
          !source["speaker_id"].is_string() ||
          !speaker_ids.insert(source["speaker_id"].get<std::string>()).second ||
          !source.contains("source_audio_stream_id") ||
          !source["source_audio_stream_id"].is_string() ||
          !source.contains("source_audio_stream_ids") ||
          !source["source_audio_stream_ids"].is_array() ||
          source["source_audio_stream_ids"].empty()) {
        add_microphone_provenance_error(
            report, registry, "/transcript/transcript.json/speaker_sources",
            "Each microphone speaker mapping requires a unique speaker_id, a primary source, and a non-empty source group.");
        break;
      }
      const std::string primary =
          source["source_audio_stream_id"].get<std::string>();
      bool primary_in_group = false;
      for (const auto& grouped_source : source["source_audio_stream_ids"]) {
        if (grouped_source.is_string() &&
            grouped_source.get<std::string>() == primary) {
          primary_in_group = true;
        }
      }
      if (!primary_in_group) {
        add_microphone_provenance_error(
            report, registry, "/transcript/transcript.json/speaker_sources",
            "A microphone speaker source group must contain its primary source.");
        break;
      }
    }
  }

  const std::vector<nlohmann::json> processors = read_jsonl_entry(
      package_path, layout, "provenance/processors.jsonl");
  const nlohmann::json* processor = nullptr;
  for (const auto& candidate : processors) {
    if (candidate.value("id", "") == processor_id) {
      processor = &candidate;
      break;
    }
  }
  if (processor == nullptr) {
    add_microphone_provenance_error(
        report, registry, "/provenance/processors.jsonl",
        "Microphone stream assignment processor_id does not resolve to a processor record.");
    return;
  }
  if (!processor->contains("reconciliation") ||
      !(*processor)["reconciliation"].is_object() ||
      !(*processor)["reconciliation"].contains("source_assignment_evidence") ||
      !(*processor)["reconciliation"].contains(
          "cross_anchor_chunk_content_evidence") ||
      !(*processor)["reconciliation"].contains(
          "maximum_duplicate_word_time_delta_us")) {
    add_microphone_provenance_error(
        report, registry, "/provenance/processors.jsonl",
        "Microphone processor provenance is missing reconciliation policy or evidence.");
  }

  std::set<std::string> processor_input_refs;
  if (processor->contains("input_refs") &&
      (*processor)["input_refs"].is_array()) {
    for (const auto& input_ref : (*processor)["input_refs"]) {
      if (input_ref.is_string()) {
        processor_input_refs.insert(input_ref.get<std::string>());
      }
    }
  }
  std::set<std::string> processor_source_ids;
  if (processor->contains("input_streams") &&
      (*processor)["input_streams"].is_array()) {
    for (const auto& input_stream : (*processor)["input_streams"]) {
      if (input_stream.contains("source_audio_stream_id") &&
          input_stream["source_audio_stream_id"].is_string() &&
          input_stream.contains("input_ref") &&
          input_stream["input_ref"].is_string()) {
        const std::string source_id =
            input_stream["source_audio_stream_id"].get<std::string>();
        const std::string input_ref =
            input_stream["input_ref"].get<std::string>();
        if (source_id.empty() || input_ref.empty() ||
            !processor_input_refs.contains(input_ref) ||
            !processor_source_ids.insert(source_id).second) {
          add_microphone_provenance_error(
              report, registry, "/provenance/processors.jsonl",
              "Each microphone input_stream must uniquely map a source ID to a processor input_ref.");
          break;
        }
      }
    }
  }
  if (processor_input_refs.empty() || processor_source_ids.empty()) {
    add_microphone_provenance_error(
        report, registry, "/provenance/processors.jsonl",
        "Microphone processor provenance must map source streams to staged input_refs.");
  }
  if (transcript.contains("speaker_sources") &&
      transcript["speaker_sources"].is_array()) {
    for (const auto& source : transcript["speaker_sources"]) {
      if (!source.contains("source_audio_stream_ids") ||
          !source["source_audio_stream_ids"].is_array()) {
        continue;
      }
      for (const auto& grouped_source : source["source_audio_stream_ids"]) {
        if (!grouped_source.is_string() ||
            !processor_source_ids.contains(grouped_source.get<std::string>())) {
          add_microphone_provenance_error(
              report, registry, "/transcript/transcript.json/speaker_sources",
              "Every grouped microphone speaker source must resolve through processor input_streams.");
          return;
        }
      }
    }
  }

  const std::vector<nlohmann::json> chunks = read_jsonl_entry(
      package_path, layout, "transcript/asr_chunk_provenance.jsonl");
  if (chunks.empty()) {
    add_microphone_provenance_error(
        report, registry, "/transcript/asr_chunk_provenance.jsonl",
        "Microphone assignment requires ASR chunk provenance.");
  } else {
    for (const auto& chunk : chunks) {
      if (!chunk.contains("input_ref") || !chunk["input_ref"].is_string() ||
          chunk["input_ref"].get<std::string>().empty() ||
          !processor_input_refs.contains(
              chunk["input_ref"].get<std::string>())) {
        add_microphone_provenance_error(
            report, registry, "/transcript/asr_chunk_provenance.jsonl",
            "Every microphone ASR chunk input_ref must resolve through its processor record.");
        break;
      }
    }
  }
}

void add_diarization_findings_impl(ValidationReport& report,
                                   const ValidationCodeRegistry& registry,
                                   const std::filesystem::path& package_path,
                                   const svp::package::PackageLayout& layout) {
  constexpr std::string_view entry = "transcript/transcript.json";
  const std::string entry_path = "/transcript/transcript.json";
  if (!has_entry(layout, std::string{entry})) {
    return;
  }

  const auto read_result =
      svp::package::read_package_entry(package_path, std::string{entry});
  if (!read_result.has_value()) {
    add_finding(report, make_finding(registry, kCodeDiarizationUnavailable,
                                     entry_path,
                                     "transcript/transcript.json exists in layout but "
                                     "cannot be read: " + read_result.error_message()));
    return;
  }

  nlohmann::json transcript;
  try {
    transcript = nlohmann::json::parse(read_result.value());
  } catch (const nlohmann::json::exception& error) {
    add_finding(report, make_finding(registry, kCodeDiarizationUnavailable,
                                     entry_path,
                                     "transcript/transcript.json is not valid JSON: " +
                                         std::string(error.what())));
    return;
  }

  std::string diarization_status;
  if (transcript.contains("diarization") &&
      transcript["diarization"].contains("status") &&
      transcript["diarization"]["status"].is_string()) {
    diarization_status = transcript["diarization"]["status"].get<std::string>();
  }

  std::size_t speaker_count = 0;
  if (transcript.contains("speaker_count") &&
      transcript["speaker_count"].is_number()) {
    speaker_count = transcript["speaker_count"].get<std::size_t>();
  }

  if (diarization_status == "fallback_one_speaker") {
    add_finding(report, make_finding(registry, kCodeDiarizationFallback,
                                     "/transcript/transcript.json",
                                     "Speaker diarization did not run. Speaker count is "
                                     "unverified fallback, not real diarization. Install "
                                     "sherpa-onnx and rebuild."));
  } else if (diarization_status == "unavailable" ||
             (speaker_count == 0 && diarization_status != "ran" &&
              diarization_status != "microphone_stream_assignment")) {
    add_finding(report, make_finding(registry, kCodeDiarizationUnavailable,
                                     "/transcript/transcript.json",
                                     "Speaker diarization is unavailable. Package does not "
                                     "contain speaker data. Install sherpa-onnx and rebuild."));
  } else if (diarization_status == "microphone_stream_assignment") {
    validate_microphone_provenance(report, registry, package_path, layout,
                                   transcript, speaker_count);
  }
}

}  // namespace

void add_diarization_findings(ValidationReport& report,
                              const ValidationCodeRegistry& registry,
                              const std::filesystem::path& package_path,
                              const svp::package::PackageLayout& layout) {
  try {
    add_diarization_findings_impl(report, registry, package_path, layout);
  } catch (const std::exception& error) {
    add_finding(report, make_finding(registry, kCodeDiarizationUnavailable,
                                     "/transcript/transcript.json",
                                     std::string("Unexpected error during diarization "
                                                 "validation: ") + error.what()));
  }
}

}  // namespace svp::validation
