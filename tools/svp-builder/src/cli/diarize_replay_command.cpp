#include "cli_context.hpp"

#include "svp/audio/sherpa_diarization.hpp"
#include "svp/audio/microphone_transcript.hpp"
#include "svp/core/memory_diagnostics.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
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
    word.confidence = record.value("confidence", 0.0);
    word.chunk_ordinal =
        record.value("chunk_ordinal", static_cast<std::int64_t>(0));
    words.push_back(std::move(word));
  }
  return words;
}

std::vector<float> aggregate_voice_fingerprint(
    const svp::audio::SherpaDiarizationResult& diarization) {
  std::map<int32_t, double> duration_by_speaker;
  for (const auto& segment : diarization.segments) {
    if (segment.speaker_id >= 0) {
      duration_by_speaker[segment.speaker_id] +=
          std::max(0.0f, segment.end_sec - segment.start_sec);
    }
  }
  std::vector<float> aggregate;
  double total_weight = 0.0;
  for (const auto& [speaker, duration] : duration_by_speaker) {
    if (duration <= 0.0 || static_cast<std::size_t>(speaker) >=
                               diarization.final_speaker_fingerprints.size()) {
      continue;
    }
    const auto& fingerprint =
        diarization.final_speaker_fingerprints[static_cast<std::size_t>(speaker)];
    if (fingerprint.empty()) continue;
    if (aggregate.empty()) aggregate.assign(fingerprint.size(), 0.0f);
    if (aggregate.size() != fingerprint.size()) continue;
    for (std::size_t index = 0; index < fingerprint.size(); ++index) {
      aggregate[index] += fingerprint[index] * static_cast<float>(duration);
    }
    total_weight += duration;
  }
  if (aggregate.empty() || total_weight <= 0.0) return {};
  double norm_squared = 0.0;
  for (float value : aggregate) norm_squared += value * value;
  const double norm = std::sqrt(norm_squared);
  if (norm <= 0.0) return {};
  for (float& value : aggregate) value /= static_cast<float>(norm);
  return aggregate;
}

std::vector<svp::audio::MicrophoneVoiceTrack> replay_voice_tracks(
    const svp::audio::SherpaDiarizationResult& diarization) {
  std::vector<svp::audio::MicrophoneVoiceTrack> tracks;
  for (int32_t speaker = 0; speaker < diarization.final_speaker_count;
       ++speaker) {
    if (static_cast<std::size_t>(speaker) >=
        diarization.final_speaker_fingerprints.size()) {
      continue;
    }
    const auto& fingerprint =
        diarization.final_speaker_fingerprints[static_cast<std::size_t>(speaker)];
    if (fingerprint.empty()) continue;
    svp::audio::MicrophoneVoiceTrack track;
    track.track_ordinal = static_cast<std::size_t>(speaker);
    track.fingerprint = fingerprint;
    for (const auto& segment : diarization.segments) {
      if (segment.speaker_id == speaker && segment.end_sec > segment.start_sec) {
        track.speech_segments.push_back({
            static_cast<std::int64_t>(std::llround(segment.start_sec * 1000000.0)),
            static_cast<std::int64_t>(std::llround(segment.end_sec * 1000000.0)),
        });
      }
    }
    if (!track.speech_segments.empty()) tracks.push_back(std::move(track));
  }
  return tracks;
}

std::vector<svp::audio::TimeSpan> replay_speech_spans(
    const std::vector<svp::audio::MicrophoneVoiceTrack>& tracks) {
  std::vector<svp::audio::TimeSpan> spans;
  for (const auto& track : tracks) {
    spans.insert(spans.end(), track.speech_segments.begin(),
                 track.speech_segments.end());
  }
  return spans;
}

std::map<std::string, std::pair<std::string, std::size_t>>
microphone_sources_by_speaker(const std::filesystem::path& processors_path) {
  std::map<std::string, std::pair<std::string, std::size_t>> sources;
  for (const auto& processor : read_jsonl_records(processors_path)) {
    if (processor.value("id", "") !=
        "proc_microphone_stream_assignment_0001") {
      continue;
    }
    const auto& speakers = processor["reconciliation"]["speakers"];
    for (const auto& speaker : speakers) {
      sources[speaker.value("speaker_id", "")] = {
          speaker.value("source_audio_stream_id", ""),
          speaker.value("source_ordinal", static_cast<std::size_t>(0))};
    }
  }
  return sources;
}

std::size_t source_ordinal_from_id(const std::string& source_id) {
  if (source_id.rfind("astream_", 0) != 0) {
    throw std::runtime_error("unsupported microphone source id: " + source_id);
  }
  const std::size_t one_based =
      static_cast<std::size_t>(std::stoul(source_id.substr(8)));
  if (one_based == 0) {
    throw std::runtime_error("microphone source id must be one-based");
  }
  return one_based - 1;
}

std::map<std::string, std::pair<std::string, std::size_t>>
microphone_sources_from_transcript(
    const std::filesystem::path& transcript_path) {
  std::ifstream input(transcript_path);
  if (!input) {
    throw std::runtime_error("unable to open transcript metadata: " +
                             transcript_path.string());
  }
  nlohmann::json transcript = nlohmann::json::parse(input);
  std::map<std::string, std::pair<std::string, std::size_t>> sources;
  for (const auto& source : transcript["speaker_sources"]) {
    const std::string source_id =
        source.value("source_audio_stream_id", "");
    sources[source.value("speaker_id", "")] = {
        source_id, source_ordinal_from_id(source_id)};
  }
  return sources;
}

void write_microphone_replay_words(
    const std::filesystem::path& path,
    const svp::audio::MicrophoneTranscriptResult& result) {
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("unable to write microphone replay words JSONL: " +
                             path.string());
  }
  for (std::size_t index = 0; index < result.words.size(); ++index) {
    const auto& word = result.words[index];
    output << nlohmann::json({
        {"text", word.text}, {"start_us", word.start_us},
        {"end_us", word.end_us}, {"confidence", word.confidence},
        {"chunk_ordinal", word.chunk_ordinal},
        {"speaker_id", result.word_speaker_assignments[index]},
    }).dump() << "\n";
  }
}

int run_microphone_reconciliation_replay(
    const DiarizeReplayCliOptions& options,
    const std::filesystem::path& staging_dir,
    const std::filesystem::path& model_dir) {
  const std::filesystem::path words_path =
      staging_dir / "transcript" / "words.jsonl";
  const std::filesystem::path processors_path =
      staging_dir / "provenance" / "processors.jsonl";
  if (!std::filesystem::exists(processors_path)) {
    throw std::runtime_error(
        "microphone replay requires provenance/processors.jsonl");
  }
  const auto word_records = read_jsonl_records(words_path);
  for (const auto& processor : read_jsonl_records(processors_path)) {
    if (processor.value("id", "") !=
        "proc_microphone_stream_assignment_0001") {
      continue;
    }
    const auto& reconciliation = processor["reconciliation"];
    const std::size_t input_word_count =
        reconciliation.value("input_word_count", word_records.size());
    if (input_word_count != word_records.size()) {
      throw std::runtime_error(
          "microphone replay requires pre-reconciliation per-stream words; "
          "the staged words.jsonl has already been deduplicated");
    }
  }

  auto sources = microphone_sources_by_speaker(processors_path);
  if (sources.empty()) {
    sources = microphone_sources_from_transcript(
        staging_dir / "transcript" / "transcript.json");
  }
  if (sources.empty()) {
    throw std::runtime_error(
        "microphone replay found no staged microphone source mappings");
  }

  std::map<std::size_t, svp::audio::MicrophoneTranscript> by_source;
  for (const auto& [speaker_id, source] : sources) {
    auto& transcript = by_source[source.second];
    transcript.source_audio_stream_id = source.first;
    transcript.source_ordinal = source.second;
    std::ostringstream ref;
    ref << "media/audio/analysis_stream_" << std::setw(3)
        << std::setfill('0') << source.second << "_mono_16k.wav";
    transcript.analysis_audio_ref = ref.str();
  }

  for (const auto& record : word_records) {
    const auto found = sources.find(record.value("speaker_id", ""));
    if (found == sources.end()) continue;
    svp::audio::AsrWord word;
    word.text = record.value("text", "");
    word.start_us = record.value("start_us", static_cast<std::int64_t>(0));
    word.end_us = record.value("end_us", static_cast<std::int64_t>(0));
    word.confidence = record.value("confidence", 0.0);
    word.chunk_ordinal =
        record.value("chunk_ordinal", static_cast<std::int64_t>(0));
    by_source[found->second.second].words.push_back(std::move(word));
  }

  std::vector<svp::audio::MicrophoneTranscript> transcripts;
  for (auto& [source_ordinal, transcript] : by_source) {
    const std::filesystem::path wav_path =
        staging_dir / transcript.analysis_audio_ref;
    if (!std::filesystem::exists(wav_path)) {
      throw std::runtime_error("microphone replay WAV not found: " +
                               wav_path.string());
    }
    if (!options.skip_fingerprints) {
      const svp::audio::SherpaDiarizationResult diarization =
          svp::audio::run_sherpa_diarization(wav_path, model_dir);
      if (!diarization.ran) {
        throw std::runtime_error("microphone replay diarization failed for " +
                                 wav_path.string());
      }
      transcript.voice_fingerprint =
          aggregate_voice_fingerprint(diarization);
      transcript.voice_tracks = replay_voice_tracks(diarization);
    }
    transcript.word_signal_db =
        svp::audio::measure_word_signal_db(wav_path, transcript.words);
    transcript.signal_profile =
        svp::audio::measure_microphone_signal_profile(
            wav_path, replay_speech_spans(transcript.voice_tracks));
    transcripts.push_back(std::move(transcript));
  }

  const svp::audio::MicrophoneTranscriptResult result =
      svp::audio::reconcile_microphone_transcripts(transcripts);
  if (!options.out_words_jsonl_path.empty()) {
    write_microphone_replay_words(options.out_words_jsonl_path, result);
  }

  nlohmann::json speakers = nlohmann::json::array();
  for (const auto& speaker : result.speakers) {
    speakers.push_back({
        {"speaker_id", speaker.speaker_id},
        {"source_audio_stream_id", speaker.source_audio_stream_id},
        {"source_audio_stream_ids", speaker.source_audio_stream_ids},
        {"source_ordinal", speaker.source_ordinal},
        {"word_count", speaker.word_count},
    });
  }
  nlohmann::json voice_matches = nlohmann::json::array();
  for (const auto& evidence : result.voice_match_evidence) {
    voice_matches.push_back({
        {"left_source_ordinal", evidence.left_source_ordinal},
        {"right_source_ordinal", evidence.right_source_ordinal},
        {"shared_speech_overlap_ratio", evidence.shared_speech_overlap_ratio},
        {"aligned_voice_coverage_ratio",
         evidence.aligned_voice_coverage_ratio},
        {"aligned_voice_match_ratio", evidence.aligned_voice_match_ratio},
    });
  }
  nlohmann::json source_quality = nlohmann::json::array();
  for (const auto& evidence : result.source_quality_evidence) {
    source_quality.push_back({
        {"source_ordinal", evidence.source_ordinal},
        {"median_word_signal_db", evidence.median_word_signal_db},
        {"noise_floor_db", evidence.noise_floor_db},
        {"median_speech_snr_db", evidence.median_speech_snr_db},
        {"mean_asr_confidence", evidence.mean_asr_confidence},
    });
  }
  nlohmann::json discarded_pairs = nlohmann::json::array();
  for (const auto& [source, anchors] :
       result.discarded_word_count_by_source_pair) {
    for (const auto& [anchor, count] : anchors) {
      discarded_pairs.push_back({{"source_ordinal", source},
                                 {"anchor_source_ordinal", anchor},
                                 {"word_count", count}});
    }
  }
  nlohmann::json source_assignments = nlohmann::json::array();
  for (const auto& assignment : result.source_assignment_evidence) {
    source_assignments.push_back({
        {"source_ordinal", assignment.source_ordinal},
        {"decision", assignment.decision},
        {"anchor_source_ordinal",
         assignment.anchor_source_ordinal.has_value()
             ? nlohmann::json(*assignment.anchor_source_ordinal)
             : nlohmann::json(nullptr)},
        {"matched_stronger_source_ordinals",
         assignment.matched_stronger_source_ordinals},
    });
  }
  std::cout << nlohmann::json({
      {"ran", true},
      {"mode", "microphone_reconciliation"},
      {"asr_ran", false},
      {"fingerprints_available", !options.skip_fingerprints},
      {"input_microphone_count", transcripts.size()},
      {"input_word_count", result.input_word_count},
      {"output_word_count", result.words.size()},
      {"discarded_cross_anchor_bleed_word_count",
       result.discarded_cross_anchor_bleed_word_count},
      {"speaker_count", result.speakers.size()},
      {"speakers", speakers},
      {"source_assignments", source_assignments},
      {"voice_matches", voice_matches},
      {"source_quality", source_quality},
      {"discarded_word_count_by_source_pair", discarded_pairs},
      {"out_words_jsonl", options.out_words_jsonl_path},
  }).dump(2) << "\n";
  return 0;
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
  if (!fs::exists(words_path)) {
    std::cerr << "svp-builder diarize-replay: words JSONL not found: "
              << words_path << "\n";
    return 1;
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

  try {
    if (options.microphone_reconciliation) {
      if (!options.skip_fingerprints) {
        if (!fs::exists(model_dir)) {
          throw std::runtime_error("model dir not found: " +
                                   model_dir.string());
        }
        if (!options.sherpa_lib_path.empty()) {
          svp::audio::set_sherpa_lib_path(options.sherpa_lib_path);
        }
        if (!svp::audio::is_sherpa_diarization_available()) {
          throw std::runtime_error("sherpa-onnx not available");
        }
      }
      return run_microphone_reconciliation_replay(options, staging_dir,
                                                  model_dir);
    }
    if (!fs::exists(wav_path)) {
      throw std::runtime_error("analysis WAV not found: " + wav_path.string());
    }
    if (!fs::exists(segments_path)) {
      throw std::runtime_error("speaker segments JSONL not found: " +
                               segments_path.string());
    }
    if (!fs::exists(model_dir)) {
      throw std::runtime_error("model dir not found: " + model_dir.string());
    }
    if (!options.sherpa_lib_path.empty()) {
      svp::audio::set_sherpa_lib_path(options.sherpa_lib_path);
    }
    if (!svp::audio::is_sherpa_diarization_available()) {
      throw std::runtime_error("sherpa-onnx not available");
    }
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
