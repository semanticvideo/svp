#include "embedded_inspection_output.hpp"

#include "svp/package/svpi_embedding_profile.hpp"
#include "svp/query/query_ops.hpp"

#include <iostream>

namespace embedded_inspection_output {

nlohmann::json embedding_json(
    const svp::package::EmbeddedSvpiInspection& inspection) {
  nlohmann::json issues = nlohmann::json::array();
  for (const auto& issue : inspection.issues) {
    issues.push_back({
        {"code", svp::package::to_string(issue.code)},
        {"offset", issue.offset},
        {"message", issue.message},
    });
  }
  nlohmann::json result = {
      {"detected", !inspection.embeddings.empty()},
      {"profile", std::string{svp::package::kSvpiMp4ProfileName}},
      {"uuid", std::string{svp::package::kSvpiMp4UuidText}},
      {"mp4_structure_valid", inspection.mp4_structure_valid},
      {"top_level_box_count", inspection.top_level_box_count},
      {"scanner_bytes_read", inspection.scanner_bytes_read},
      {"issues", std::move(issues)},
  };
  if (!inspection.embeddings.empty()) {
    const auto& info = inspection.embeddings.front();
    result["profile_version"] = info.profile_version;
    result["box_offset"] = info.box_offset;
    result["box_size"] = info.box_size;
    result["payload_offset"] = info.payload_offset;
    result["payload_size"] = info.payload_size;
    result["payload_hash_status"] = !info.hash_verified
        ? "not_verified" : info.hash_matches ? "match" : "mismatch";
    result["embedding_count"] = inspection.embeddings.size();
  }
  return result;
}

nlohmann::json package_summary_json(
    const svp::package::PackageSummary& summary) {
  return {
      {"layout_readable", summary.layout_readable},
      {"entry_count", summary.entry_count},
      {"manifest", {
          {"svp_version", summary.manifest.svp_version},
          {"package_id", summary.manifest.package_id},
          {"primary_media_id", summary.manifest.primary_media_id},
      }},
      {"svpi", {
          {"is_svpi", summary.svpi.is_svpi},
          {"svpi_version", summary.svpi.svpi_version},
          {"media_binding_ref", summary.svpi.media_binding_ref},
      }},
      {"text", {
          {"text_region_count", summary.text.text_regions.record_count},
          {"text_observation_count", summary.text.text_observations.record_count},
          {"numeric_value_count", summary.text.numeric_values.record_count},
      }},
      {"colors", {
          {"color_observation_count", summary.colors.color_observations.record_count},
          {"color_space", summary.colors.color_space},
      }},
      {"index", {
          {"sqlite_present", summary.index.sqlite_present},
          {"schema_version", summary.index.index_schema_version},
      }},
  };
}

void print_embedding(const svp::package::EmbeddedSvpiInspection& inspection) {
  std::cout << "Embedded SVPI transport\n";
  std::cout << "  detected: " << (!inspection.embeddings.empty() ? "yes" : "no") << "\n";
  std::cout << "  mp4_structure_valid: "
            << (inspection.mp4_structure_valid ? "yes" : "no") << "\n";
  std::cout << "  scanner_bytes_read: " << inspection.scanner_bytes_read << "\n";
  if (!inspection.embeddings.empty()) {
    const auto& info = inspection.embeddings.front();
    std::cout << "  profile_version: " << info.profile_version << "\n";
    std::cout << "  uuid: " << svp::package::kSvpiMp4UuidText << "\n";
    std::cout << "  box_offset: " << info.box_offset << "\n";
    std::cout << "  box_size: " << info.box_size << "\n";
    std::cout << "  payload_offset: " << info.payload_offset << "\n";
    std::cout << "  payload_size: " << info.payload_size << "\n";
    std::cout << "  payload_hash: "
              << (!info.hash_verified ? "not verified"
                                      : info.hash_matches ? "match" : "mismatch")
              << "\n";
  }
  for (const auto& issue : inspection.issues) {
    std::cout << "  " << svp::package::to_string(issue.code)
              << " at " << issue.offset << ": " << issue.message << "\n";
  }
}

nlohmann::json semantic_summary_json(
    const std::filesystem::path& package_path) {
  const auto transcript = svp::query::transcript_summary(package_path);
  const auto relationships = svp::query::relationship_summary(package_path);
  return {
      {"transcript", {
          {"present", transcript.present},
          {"parsed", transcript.parsed},
          {"word_count", transcript.word_count_file},
          {"speaker_count", transcript.speaker_count_file},
          {"speech_region_count", transcript.speech_region_count},
      }},
      {"relationships", {
          {"present", relationships.present},
          {"readable", relationships.readable},
          {"total_count", relationships.total_count},
          {"support_count", relationships.support_count},
          {"semantic_count", relationships.semantic_count},
          {"unknown_count", relationships.unknown_count},
      }},
  };
}

void print_semantic_summary(const std::filesystem::path& package_path) {
  const auto transcript = svp::query::transcript_summary(package_path);
  std::cout << "\nTranscript\n";
  std::cout << "  present: " << (transcript.present ? "yes" : "no") << "\n";
  if (transcript.present) {
    std::cout << "  words: " << transcript.word_count_file << "\n";
    std::cout << "  speakers: " << transcript.speaker_count_file << "\n";
    std::cout << "  speech_regions: " << transcript.speech_region_count << "\n";
  }
  const auto relationships = svp::query::relationship_summary(package_path);
  std::cout << "\nRelationships\n";
  std::cout << "  present: " << (relationships.present ? "yes" : "no") << "\n";
  if (relationships.present) {
    std::cout << "  total: " << relationships.total_count << "\n";
    std::cout << "  support: " << relationships.support_count << "\n";
    std::cout << "  semantic: " << relationships.semantic_count << "\n";
  }
}

}  // namespace embedded_inspection_output
