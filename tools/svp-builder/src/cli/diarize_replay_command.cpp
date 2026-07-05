#include "cli_context.hpp"

#include "svp/audio/sherpa_diarization.hpp"
#include "svp/core/memory_diagnostics.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <unistd.h>
#endif

namespace {

std::uint64_t replay_memory_limit_bytes() {
  const char* value = std::getenv("SVP_BUILDER_MEMORY_LIMIT_MB");
  if (value != nullptr && *value != '\0') {
    char* end = nullptr;
    const unsigned long long mb = std::strtoull(value, &end, 10);
    if (end != value && mb > 0) {
      return static_cast<std::uint64_t>(mb) * 1024ULL * 1024ULL;
    }
  }
  return 5ULL * 1024ULL * 1024ULL * 1024ULL;
}

int32_t speaker_index_from_id(const std::string& speaker_id) {
  if (speaker_id.rfind("speaker_", 0) != 0) return -1;
  try {
    const int32_t one_based = std::stoi(speaker_id.substr(8));
    return one_based - 1;
  } catch (...) {
    return -1;
  }
}

std::vector<nlohmann::json> read_jsonl_records(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("unable to open JSONL: " + path.string());
  }

  std::vector<nlohmann::json> records;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) continue;
    records.push_back(nlohmann::json::parse(line));
  }
  return records;
}

std::vector<svp::audio::AsrWord> parse_words(
    const std::vector<nlohmann::json>& records) {
  std::vector<svp::audio::AsrWord> words;
  words.reserve(records.size());
  for (const auto& record : records) {
    svp::audio::AsrWord word;
    word.text = record.value("text", "");
    word.start_us = record.value("start_us", static_cast<std::int64_t>(0));
    word.end_us = record.value("end_us", static_cast<std::int64_t>(0));
    words.push_back(std::move(word));
  }
  return words;
}

svp::audio::SherpaDiarizationResult parse_diarization_result(
    const std::vector<nlohmann::json>& records) {
  svp::audio::SherpaDiarizationResult result;
  result.ran = true;
  result.reconciliation_method = "staged_diarization_replay";

  int32_t max_speaker = -1;
  result.segments.reserve(records.size());
  for (const auto& record : records) {
    const std::string speaker_id = record.value("speaker_id", "");
    const int32_t speaker_index = speaker_index_from_id(speaker_id);
    if (speaker_index < 0) continue;

    svp::audio::SherpaDiarizationSegment segment;
    segment.start_sec =
        static_cast<float>(record.value("start_us", static_cast<std::int64_t>(0))) /
        1000000.0f;
    segment.end_sec =
        static_cast<float>(record.value("end_us", static_cast<std::int64_t>(0))) /
        1000000.0f;
    segment.speaker_id = speaker_index;
    result.segments.push_back(segment);
    max_speaker = std::max(max_speaker, speaker_index);
  }

  result.final_speaker_count = max_speaker + 1;
  return result;
}

void write_replayed_words(const std::filesystem::path& path,
                          std::vector<nlohmann::json> records,
                          const std::vector<std::string>& assignments) {
  if (records.size() != assignments.size()) {
    throw std::runtime_error("assignment count does not match words record count");
  }
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("unable to write replayed words JSONL: " + path.string());
  }
  for (std::size_t i = 0; i < records.size(); ++i) {
    records[i]["speaker_id"] = assignments[i];
    output << records[i].dump() << "\n";
  }
}

}  // namespace

int run_diarize_replay_command(const DiarizeReplayCliOptions& options) {
  namespace fs = std::filesystem;

  const fs::path staging_dir(options.staging_dir);
  const fs::path model_dir(options.model_dir);
  const fs::path wav_path =
      staging_dir / "media" / "audio" / "analysis_mono_16k.wav";
  const fs::path words_path = staging_dir / "transcript" / "words.jsonl";
  const fs::path segments_path =
      staging_dir / "transcript" / "speaker_segments.jsonl";

  if (!fs::exists(staging_dir)) {
    std::cerr << "svp-builder diarize-replay: staging dir not found: "
              << staging_dir << "\n";
    return 1;
  }
  if (!fs::exists(wav_path)) {
    std::cerr << "svp-builder diarize-replay: analysis WAV not found: "
              << wav_path << "\n";
    return 1;
  }
  if (!fs::exists(words_path)) {
    std::cerr << "svp-builder diarize-replay: words JSONL not found: "
              << words_path << "\n";
    return 1;
  }
  if (!fs::exists(segments_path)) {
    std::cerr << "svp-builder diarize-replay: speaker segments JSONL not found: "
              << segments_path << "\n";
    return 1;
  }
  if (!fs::exists(model_dir)) {
    std::cerr << "svp-builder diarize-replay: model dir not found: "
              << model_dir << "\n";
    return 1;
  }

  if (!options.sherpa_lib_path.empty()) {
    svp::audio::set_sherpa_lib_path(options.sherpa_lib_path);
  }

  std::ostringstream diag_name;
  diag_name << "svp-diarize-replay-memory";
#if defined(__APPLE__)
  diag_name << "-" << static_cast<long long>(getpid());
#endif
  diag_name << ".jsonl";
  const fs::path diag_dir = fs::current_path() / "build" / "diagnostics";
  const fs::path diag_path = diag_dir / diag_name.str();
  svp::core::configure_memory_diagnostics(diag_path, replay_memory_limit_bytes());

  if (!svp::audio::is_sherpa_diarization_available()) {
    std::cerr << "svp-builder diarize-replay: sherpa-onnx not available\n";
    return 1;
  }

  try {
    const std::vector<nlohmann::json> word_records =
        read_jsonl_records(words_path);
    const std::vector<nlohmann::json> segment_records =
        read_jsonl_records(segments_path);
    const std::vector<svp::audio::AsrWord> words = parse_words(word_records);
    const svp::audio::SherpaDiarizationResult diar_result =
        parse_diarization_result(segment_records);

    svp::core::trace_memory_event("diarize_replay.start", {
        {"staging_dir", staging_dir.string()},
        {"word_count", std::to_string(words.size())},
        {"segment_count", std::to_string(diar_result.segments.size())},
        {"speaker_count", std::to_string(diar_result.final_speaker_count)}
    });

    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<std::string> assignments =
        svp::audio::replay_word_speaker_assignments(
            wav_path, model_dir, words, diar_result);
    const auto t1 = std::chrono::steady_clock::now();
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    svp::core::trace_memory_event("diarize_replay.end", {
        {"elapsed_ms", std::to_string(elapsed_ms)},
        {"assignment_count", std::to_string(assignments.size())}
    });

    if (assignments.size() != words.size()) {
      std::cerr << "svp-builder diarize-replay: replay did not produce word assignments\n";
      return 1;
    }

    if (!options.out_words_jsonl_path.empty()) {
      write_replayed_words(options.out_words_jsonl_path, word_records, assignments);
    }

    std::cout << "{\n";
    std::cout << "  \"ran\": true,\n";
    std::cout << "  \"elapsed_ms\": " << elapsed_ms << ",\n";
    std::cout << "  \"word_count\": " << words.size() << ",\n";
    std::cout << "  \"segment_count\": " << diar_result.segments.size() << ",\n";
    std::cout << "  \"final_speaker_count\": "
              << diar_result.final_speaker_count << ",\n";
    std::cout << "  \"diagnostics_log\": \"" << diag_path.string() << "\"";
    if (!options.out_words_jsonl_path.empty()) {
      std::cout << ",\n  \"out_words_jsonl\": \""
                << options.out_words_jsonl_path << "\"\n";
    } else {
      std::cout << "\n";
    }
    std::cout << "}\n";
  } catch (const std::exception& ex) {
    std::cerr << "svp-builder diarize-replay: " << ex.what() << "\n";
    return 1;
  }
  return 0;
}
