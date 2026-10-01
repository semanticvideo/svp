#pragma once

// How a build composes provenance/processors.jsonl (RC2 §18.1: every
// processor writes a record there) from stages that run as separate tasks.
//
// No lane stage writes provenance/processors.jsonl. A stage that emits
// processor records writes them to its own fragment,
// provenance/stage_processors/<stage>.jsonl, inside its own staging scope;
// the vision lane hands its records to the join through its task states.
// The package entities stage, which runs after every lane, composes
// processors.jsonl from every fragment plus the vision records, then removes
// the fragments. No stage can drop another stage's records, and the result
// does not depend on which task finished first.
//
// A --stop-after build has no entities stage; its fragment stays in staging.

#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace svp::builder {

// Staging directory of the per-stage fragments (a staging scope prefix).
inline constexpr std::string_view kStageProcessorFragmentDir =
    "provenance/stage_processors/";

// The stages that write a fragment, one fragment each.
namespace processor_fragment {
inline constexpr std::string_view kColor = "color";
inline constexpr std::string_view kFoundationOcr = "foundation_ocr";
inline constexpr std::string_view kAudioExtract = "audio_extract";
inline constexpr std::string_view kAudioTranscribe = "audio_transcribe";
}  // namespace processor_fragment

// "provenance/stage_processors/<stage>.jsonl", relative to staging.
[[nodiscard]] std::string stage_processor_fragment_ref(std::string_view stage);
[[nodiscard]] std::filesystem::path stage_processor_fragment_path(
    const std::filesystem::path& staging_dir, std::string_view stage);

// Replaces `stage`'s fragment with `records`.
void write_stage_processor_fragment(const std::filesystem::path& staging_dir,
                                    std::string_view stage,
                                    const std::vector<nlohmann::json>& records);

struct ProcessorRecordMerge {
  std::vector<nlohmann::json> records;
  // Records dropped because another contribution carried the same id.
  std::size_t duplicates_merged = 0;
};

// One record per id, sorted by id. When contributions carry several records
// with one id, the record with the smallest compact serialization is kept
// (the rule relationship provenance uses), so neither the order of
// `contributions` nor the order within one changes the result. Records
// without a string id are dropped, as every processors.jsonl writer does.
[[nodiscard]] ProcessorRecordMerge merge_processor_records_canonically(
    const std::vector<std::vector<nlohmann::json>>& contributions);

// Writes provenance/processors.jsonl from every fragment in staging and
// `lane_records`, replacing any earlier file, and removes the fragment
// directory. Returns the merge it wrote.
ProcessorRecordMerge compose_processor_provenance(
    const std::filesystem::path& staging_dir,
    const std::vector<nlohmann::json>& lane_records);

}  // namespace svp::builder
