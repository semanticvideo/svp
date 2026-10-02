// asr.chunk_batch and diarize.window (M5): parameter schemas, task
// partitioning that scales with the audio, and, with real models, that work
// run as tasks folds into exactly the result of the same work run in one
// pass: ASR chunks and diarization windows are pure functions of their
// samples, whichever process ran them and in whatever order.
//
// The model tests run only when the model cache holds the bundles they need
// (SVP_MODEL_CACHE_DIR, else SVP_MODEL_CACHE_ROOT); diarization also needs
// a loadable sherpa-onnx library. Otherwise they report a skip.

#include "svp/audio/asr_execution_boundary.hpp"
#include "svp/audio/diarization_boundary.hpp"
#include "svp/audio/sherpa_diarization.hpp"
#include "svp/audio/tasks/asr_chunk_batch.hpp"
#include "svp/audio/tasks/audio_task_environment.hpp"
#include "svp/audio/tasks/diarize_window.hpp"
#include "svp/audio/whisper_model.hpp"
#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/source_fingerprint.hpp"
#include "svp/models/reference_processor_model_ids.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace tasks = svp::audio::tasks;

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename Function>
void require_throws(Function function, const std::string& message) {
  try {
    function();
  } catch (const std::exception&) {
    return;
  }
  throw std::runtime_error(message);
}

std::optional<std::filesystem::path> model_cache() {
  for (const char* name : {"SVP_MODEL_CACHE_DIR", "SVP_MODEL_CACHE_ROOT"}) {
    if (const char* value = std::getenv(name); value != nullptr && value[0] != '\0') {
      return std::filesystem::path(value);
    }
  }
  return std::nullopt;
}

svp::audio::WhisperRuntimeThreads explicit_whisper_threads() {
  svp::audio::WhisperRuntimeThreads threads;
  threads.whisper = {.decode = 4, .vad = 1};
  threads.forced_alignment = {.intra_op = 1, .inter_op = 1};
  return threads;
}

// --- WAV fixtures -----------------------------------------------------------

std::vector<char> pcm_data(const std::filesystem::path& wav) {
  std::ifstream file(wav, std::ios::binary);
  const std::vector<char> bytes((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
  require(bytes.size() > 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0,
          "not a RIFF file: " + wav.string());
  std::size_t offset = 12;
  while (offset + 8 <= bytes.size()) {
    std::uint32_t size = 0;
    std::memcpy(&size, bytes.data() + offset + 4, 4);
    if (std::memcmp(bytes.data() + offset, "data", 4) == 0) {
      const std::size_t end = std::min(bytes.size(), offset + 8 + size);
      return {bytes.begin() + static_cast<std::ptrdiff_t>(offset + 8),
              bytes.begin() + static_cast<std::ptrdiff_t>(end & ~std::size_t{1})};
    }
    offset += 8 + size + (size % 2);
  }
  throw std::runtime_error("no data chunk in " + wav.string());
}

void write_u32(std::ofstream& out, std::uint32_t value) { out.write(reinterpret_cast<const char*>(&value), 4); }
void write_u16(std::ofstream& out, std::uint16_t value) { out.write(reinterpret_cast<const char*>(&value), 2); }

// The repository's 16 kHz mono speech fixtures, concatenated `repeats` times:
// longer audio than any one fixture, so the plan has many chunks and
// several diarization windows.
std::filesystem::path long_speech_wav(const std::filesystem::path& directory, int repeats) {
  const std::filesystem::path fixtures =
      std::filesystem::path(SVP_REPO_ROOT) / "fixtures/audio/sherpa-diarization";
  std::vector<char> data;
  for (int repeat = 0; repeat < repeats; ++repeat) {
    for (const char* name : {"four-speaker.wav", "three-speaker.wav", "two-speaker.wav",
                             "one-speaker.wav"}) {
      const std::vector<char> part = pcm_data(fixtures / name);
      data.insert(data.end(), part.begin(), part.end());
    }
  }
  std::filesystem::create_directories(directory);
  const std::filesystem::path path = directory / "long-speech.wav";
  std::ofstream out(path, std::ios::binary);
  out.write("RIFF", 4);
  write_u32(out, static_cast<std::uint32_t>(36 + data.size()));
  out.write("WAVEfmt ", 8);
  write_u32(out, 16);
  write_u16(out, 1);
  write_u16(out, 1);
  write_u32(out, 16000);
  write_u32(out, 32000);
  write_u16(out, 2);
  write_u16(out, 16);
  out.write("data", 4);
  write_u32(out, static_cast<std::uint32_t>(data.size()));
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  return path;
}

// --- An in-process runtime for the task types ---------------------------------

class TaskRuntime {
 public:
  TaskRuntime(const std::filesystem::path& model_cache, const std::filesystem::path& scratch,
              bool record_start_failures) {
    tasks::register_audio_tasks(
        registry_, tasks::AudioTaskEnvironment{
                       .model_cache_root = model_cache,
                       .model_cache_for = {},
                       .scratch_dir = scratch,
                       .write_output =
                           [this](std::span<const std::byte> bytes, std::string media_type,
                                  std::string role) {
                             const std::scoped_lock lock(mutex_);
                             svp::exec::ArtifactRef ref{.blake3 = svp::exec::blake3_digest(bytes),
                                                        .bytes = bytes.size(),
                                                        .media_type = std::move(media_type),
                                                        .role = std::move(role)};
                             stored_[svp::exec::blake3_hex(ref.blake3)] =
                                 std::vector<std::byte>(bytes.begin(), bytes.end());
                             return ref;
                           },
                       .record_start_failures = record_start_failures});
  }

  // Runs `spec` with `wav` as its input; returns its output payloads.
  std::pair<svp::exec::TaskResult, std::vector<std::vector<std::byte>>> run(
      const svp::exec::TaskSpec& spec, const std::filesystem::path& wav) {
    const svp::exec::TaskTypeDefinition* type = registry_.find(spec.task_type, spec.task_type_version);
    require(type != nullptr, "task type not registered: " + spec.task_type);
    require(!type->validate_parameters(spec.parameters), "parameters do not validate");
    svp::exec::ResolvedInputs inputs;
    inputs.emplace(std::string(tasks::kAudioTaskInput),
                   svp::exec::ResolvedInput{.ref = spec.inputs.at(std::string(tasks::kAudioTaskInput)),
                                            .path = wav});
    const svp::exec::CancellationToken never;
    svp::exec::TaskResult result = type->execute(spec, inputs, never);
    std::vector<std::vector<std::byte>> payloads;
    for (const svp::exec::ArtifactRef& output : result.outputs) {
      payloads.push_back(stored_.at(svp::exec::blake3_hex(output.blake3)));
    }
    return {std::move(result), std::move(payloads)};
  }

 private:
  svp::exec::TaskTypeRegistry registry_;
  std::mutex mutex_;
  std::map<std::string, std::vector<std::byte>> stored_;
};

svp::exec::ArtifactRef audio_ref(const std::filesystem::path& wav) {
  const svp::exec::SourceFingerprintRecord fingerprint = svp::exec::fingerprint_source("audio", wav);
  return {.blake3 = fingerprint.blake3,
          .bytes = fingerprint.size_bytes,
          .media_type = std::string(tasks::kAudioTaskInputMediaType),
          .role = std::string(tasks::kAudioTaskInputRole)};
}

svp::exec::TaskModelRef fake_ref(const std::string& model_id) {
  const svp::exec::Blake3Digest digest = svp::exec::blake3_digest(std::string_view(model_id));
  return {.model_id = model_id,
          .model_bundle_id = model_id + "@test+blake3_" + svp::exec::blake3_hex(digest).substr(0, 12),
          .bundle_blake3 = digest};
}

// --- Schema and partition tests -------------------------------------------------

tasks::AsrChunkBatchTaskInputs asr_inputs(std::int64_t duration_us, double seconds_per_chunk) {
  tasks::AsrChunkBatchTaskInputs inputs;
  inputs.build_session_id = "bs_test";
  inputs.audio = {.blake3 = svp::exec::blake3_digest(std::string_view("audio")),
                  .bytes = 1,
                  .media_type = std::string(tasks::kAudioTaskInputMediaType),
                  .role = std::string(tasks::kAudioTaskInputRole)};
  inputs.model_refs = {fake_ref(svp::models::kWhisperSmallEnglishModelId),
                       fake_ref(svp::models::kWhisperCppSileroVadModelId)};
  inputs.work.chunks = svp::audio::build_asr_chunk_plan(duration_us).chunks;
  inputs.work.model_id = svp::models::kWhisperSmallEnglishModelId;
  inputs.work.vad_model_id = svp::models::kWhisperCppSileroVadModelId;
  inputs.work.threads = explicit_whisper_threads();
  inputs.batch_policy.estimated_seconds_per_item = seconds_per_chunk;
  return inputs;
}

void test_asr_chunk_batch_parameters_round_trip_and_refuse_bad_values() {
  const tasks::AsrChunkBatchTaskInputs inputs = asr_inputs(60'000'000, 1.0);
  const svp::exec::TaskSpec spec =
      tasks::make_asr_chunk_batch_task_spec(inputs, {.first = 1, .count = 2});
  const tasks::AsrChunkBatchParameters parameters =
      tasks::asr_chunk_batch_parameters_from_json(spec.parameters);
  require(parameters.chunks.size() == 2 && parameters.chunks[0].ordinal == 1 &&
              parameters.chunks[1].chunk.chunk_id == inputs.work.chunks[2].chunk_id,
          "asr.chunk_batch parameters do not carry their chunks");
  require(tasks::asr_chunk_batch_parameters_to_json(parameters) == spec.parameters,
          "asr.chunk_batch parameters do not round-trip");

  nlohmann::json threads_left_to_host = spec.parameters;
  threads_left_to_host["threads"]["decode"] = 0;
  require(tasks::validate_asr_chunk_batch_parameters(threads_left_to_host).has_value(),
          "a thread count left to the host must be refused");
  nlohmann::json unknown = spec.parameters;
  unknown["extra"] = true;
  require(tasks::validate_asr_chunk_batch_parameters(unknown).has_value(),
          "unknown fields must be refused");
  nlohmann::json out_of_order = spec.parameters;
  std::swap(out_of_order["chunks"][0], out_of_order["chunks"][1]);
  require(tasks::validate_asr_chunk_batch_parameters(out_of_order).has_value(),
          "chunk ordinals must ascend");
  require_throws([&] { (void)tasks::make_asr_chunk_batch_task_spec(inputs, {.first = 3, .count = 9}); },
                 "a batch outside the chunks must be refused");
}

// However long the audio, every chunk lands in exactly one task, in order,
// and the task count grows with the chunk count (never a fixed count).
void test_asr_chunk_batches_cover_every_chunk_and_scale_with_duration() {
  std::size_t previous_tasks = 0;
  for (const std::int64_t minutes : {1, 10, 60, 240}) {
    const tasks::AsrChunkBatchTaskInputs inputs = asr_inputs(minutes * 60'000'000, 2.0);
    const auto batches = svp::vision::tasks::partition_items(
        inputs.work.chunks.size(), inputs.batch_policy, [&](std::size_t index) {
          return tasks::asr_chunk_item_bytes(inputs.work.chunks[index], index);
        });
    std::uint64_t next = 0;
    std::set<std::string> ids;
    for (const auto& batch : batches) {
      require(batch.first == next, "chunk batches must be contiguous");
      const svp::exec::TaskSpec spec = tasks::make_asr_chunk_batch_task_spec(inputs, batch);
      require(ids.insert(spec.task_id).second, "task IDs must be unique");
      next += batch.count;
    }
    require(next == inputs.work.chunks.size(), "every chunk must be in a task");
    require(batches.size() >= previous_tasks, "longer audio must not have fewer tasks");
    previous_tasks = batches.size();
  }
  require(previous_tasks > 1, "four hours of audio must be more than one task");
}

void test_diarize_window_parameters_round_trip_and_refuse_bad_values() {
  tasks::DiarizeWindowTaskInputs inputs;
  inputs.build_session_id = "bs_test";
  inputs.audio = asr_inputs(1, 1.0).audio;
  inputs.model_ref = fake_ref(svp::models::kSherpaOnnxDiarizationModelId);
  // 20 minutes of 16 kHz audio: four windows.
  inputs.work.sample_count = 16000ULL * 60 * 20;
  inputs.work.window_count = svp::audio::diarization_window_count(inputs.work.sample_count);
  inputs.work.settings = {.threads = {.segmentation = 1, .embedding = 1},
                          .compute_embeddings = true};
  inputs.sherpa_library = svp::exec::blake3_prefixed(svp::exec::blake3_digest(std::string_view("lib")));
  require(inputs.work.window_count == 4, "20 minutes are four diarization windows");
  for (std::size_t window = 0; window < inputs.work.window_count; ++window) {
    const svp::exec::TaskSpec spec = tasks::make_diarize_window_task_spec(inputs, window);
    const tasks::DiarizeWindowParameters parameters =
        tasks::diarize_window_parameters_from_json(spec.parameters);
    require(parameters.window_index == window, "the spec must name its window");
    require(tasks::diarize_window_parameters_to_json(parameters) == spec.parameters,
            "diarize.window parameters do not round-trip");
    require(spec.resources.est_seconds >= 1, "a window has a cost estimate");
  }
  nlohmann::json bad = tasks::make_diarize_window_task_spec(inputs, 0).parameters;
  bad["window_index"] = 4;
  require(tasks::validate_diarize_window_parameters(bad).has_value(),
          "a window outside the WAV must be refused");
  bad = tasks::make_diarize_window_task_spec(inputs, 0).parameters;
  bad["threads"]["embedding"] = 0;
  require(tasks::validate_diarize_window_parameters(bad).has_value(),
          "a thread count left to the host must be refused");
  require_throws([&] { (void)tasks::make_diarize_window_task_spec(inputs, 4); },
                 "a window outside the work must be refused");
}

// --- Model tests ---------------------------------------------------------------

void require_same_words(const std::vector<svp::audio::AsrWord>& expected,
                        const std::vector<svp::audio::AsrWord>& actual, const std::string& what) {
  require(expected.size() == actual.size(), what + ": word counts differ");
  for (std::size_t index = 0; index < expected.size(); ++index) {
    const auto& left = expected[index];
    const auto& right = actual[index];
    require(left.text == right.text && left.start_us == right.start_us &&
                left.end_us == right.end_us && left.confidence == right.confidence &&
                left.chunk_ordinal == right.chunk_ordinal &&
                left.timing_source == right.timing_source,
            what + ": word " + std::to_string(index) + " differs");
  }
}

// The chunks of a long WAV run as asr.chunk_batch tasks in reverse order,
// in batches of every size from one chunk up, after other audio has warmed
// the process: the boundary's fold gives exactly the words of one plan-order
// pass with no tasks.
void test_dispatched_asr_chunks_fold_to_the_local_result() {
  const std::optional<std::filesystem::path> cache = model_cache();
  if (!cache || !svp::audio::is_whisper_runtime_available() ||
      !svp::audio::verify_asr_model_files(svp::models::kWhisperSmallEnglishModelId, *cache) ||
      !svp::audio::verify_asr_model_files(svp::models::kWhisperCppSileroVadModelId, *cache)) {
    std::cout << "SKIP test_dispatched_asr_chunks_fold_to_the_local_result: no ASR models\n";
    return;
  }
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-audio-task-tests-asr";
  std::filesystem::remove_all(root);
  const std::filesystem::path wav = long_speech_wav(root / "staging" / "media/audio", 1);
  const std::filesystem::path staging = root / "staging";
  const std::string input_ref = "media/audio/long-speech.wav";
  const std::size_t samples = svp::audio::diarization_wav_sample_count(wav);
  const std::int64_t duration_us = static_cast<std::int64_t>(samples) * 1'000'000 / 16000;
  const svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(duration_us, svp::audio::kDefaultAsrChunkDurationUs,
                                       svp::audio::kDefaultAsrChunkOverlapUs, input_ref);
  require(plan.chunks.size() > 10, "the long fixture must span many chunks");
  const auto boundary = svp::audio::build_asr_execution_boundary(plan, true, true, true, true,
                                                                 input_ref);
  const svp::audio::WhisperRuntimeThreads threads = explicit_whisper_threads();

  const svp::audio::AsrExecutionBoundary local =
      svp::audio::execute_asr_boundary(boundary, staging, *cache, threads);
  require(local.asr_status == svp::audio::AsrStatus::ran, "local ASR did not run");

  std::filesystem::create_directories(root / "scratch");
  TaskRuntime runtime(*cache, root / "scratch", false);
  std::size_t tasks_run = 0;
  const svp::audio::AsrChunkDispatch dispatch =
      [&](const svp::audio::AsrChunkWork& work, const auto&)
      -> std::optional<std::vector<std::optional<svp::audio::AsrChunkOutcome>>> {
    tasks::AsrChunkBatchTaskInputs inputs;
    inputs.build_session_id = "bs_test";
    inputs.audio = audio_ref(work.input_wav);
    inputs.model_refs = {fake_ref(work.model_id), fake_ref(work.vad_model_id)};
    if (work.alignment_model_id) {
      inputs.model_refs.push_back(fake_ref(*work.alignment_model_id));
    }
    inputs.work = work;
    inputs.batch_policy.estimated_seconds_per_item = 1.0;
    // Batches of 1, 2, 3, ... chunks, run last first.
    std::vector<svp::vision::tasks::ItemBatch> batches;
    for (std::uint64_t first = 0, size = 1; first < work.chunks.size(); first += size, ++size) {
      batches.push_back({.first = first, .count = std::min<std::uint64_t>(size, work.chunks.size() - first)});
    }
    std::vector<std::optional<svp::audio::AsrChunkOutcome>> outcomes(work.chunks.size());
    for (auto batch = batches.rbegin(); batch != batches.rend(); ++batch) {
      const svp::exec::TaskSpec spec = tasks::make_asr_chunk_batch_task_spec(inputs, *batch);
      auto [result, payloads] = runtime.run(spec, work.input_wav);
      require(result.status == svp::exec::TaskStatus::succeeded,
              "asr.chunk_batch failed: " + (result.error ? result.error->message : std::string()));
      auto part = tasks::read_asr_chunk_batch_output(spec, result.outputs, payloads);
      for (std::size_t offset = 0; offset < part.size(); ++offset) {
        require(part[offset].has_value(), "a worker-side chunk must run or fail the task");
        outcomes[batch->first + offset] = std::move(part[offset]);
      }
      ++tasks_run;
    }
    return outcomes;
  };
  const svp::audio::AsrExecutionBoundary dispatched =
      svp::audio::execute_asr_boundary(boundary, staging, *cache, threads, {}, dispatch);
  require(tasks_run > 1, "the chunks must run as several tasks");
  require(dispatched.asr_status == local.asr_status, "ASR status differs");
  require(dispatched.alignment_status == local.alignment_status, "alignment status differs");
  require(dispatched.raw_word_count == local.raw_word_count, "raw word counts differ");
  require(dispatched.blockers == local.blockers, "blockers differ");
  require_same_words(local.reconciled_words, dispatched.reconciled_words, "dispatched ASR");
  std::filesystem::remove_all(root);
  std::cout << "asr: " << plan.chunks.size() << " chunks, " << tasks_run << " tasks, "
            << local.reconciled_words.size() << " words identical\n";
}

void require_same_segments(const std::vector<svp::audio::SherpaDiarizationSegment>& left,
                           const std::vector<svp::audio::SherpaDiarizationSegment>& right,
                           const std::string& what) {
  require(left.size() == right.size(), what + ": segment counts differ");
  for (std::size_t index = 0; index < left.size(); ++index) {
    require(left[index].start_sec == right[index].start_sec &&
                left[index].end_sec == right[index].end_sec &&
                left[index].speaker_id == right[index].speaker_id,
            what + ": segment " + std::to_string(index) + " differs");
  }
}

// The windows of a WAV longer than three diarization windows are mapped as
// diarize.window tasks, last first: the fold gives exactly the diarization
// of one pass with no tasks (segments, speakers, fingerprints, word
// speakers).
void test_dispatched_diarization_windows_fold_to_the_local_result() {
  const std::optional<std::filesystem::path> cache = model_cache();
  if (!cache ||
      !svp::audio::verify_diarization_model_files(svp::models::kSherpaOnnxDiarizationModelId,
                                                  *cache) ||
      !svp::audio::is_sherpa_diarization_available()) {
    std::cout << "SKIP test_dispatched_diarization_windows_fold_to_the_local_result: "
                 "no diarization model or sherpa-onnx\n";
    return;
  }
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-audio-task-tests-diarization";
  std::filesystem::remove_all(root);
  const std::filesystem::path wav = long_speech_wav(root, 3);
  const std::size_t samples = svp::audio::diarization_wav_sample_count(wav);
  require(svp::audio::diarization_window_count(samples) >= 3,
          "the long fixture must span several windows");
  const std::filesystem::path model_dir = *cache / svp::models::kSherpaOnnxDiarizationModelId;
  const svp::models::SherpaThreadCounts threads{.segmentation = 1, .embedding = 1};
  // A few words spread over the audio, so word assignment runs too.
  std::vector<svp::audio::AsrWord> words;
  for (std::int64_t start = 1'000'000; start < static_cast<std::int64_t>(samples) * 62;
       start += 7'000'000) {
    words.push_back({.text = "word", .start_us = start, .end_us = start + 400'000,
                     .confidence = 0.5, .chunk_ordinal = 0, .timing_source = "whisper_cpp_dtw"});
  }

  const svp::audio::SherpaDiarizationResult local =
      svp::audio::run_sherpa_diarization(wav, model_dir, threads, words);
  require(local.ran, "local diarization did not run");

  std::filesystem::create_directories(root / "scratch");
  TaskRuntime runtime(*cache, root / "scratch", false);
  const std::string library = *tasks::loaded_sherpa_library_identity();
  std::size_t windows_run = 0;
  const svp::audio::DiarizationWindowDispatch dispatch =
      [&](const svp::audio::DiarizationWindowWork& work, const auto&)
      -> std::optional<std::vector<std::optional<svp::audio::DiarizationWindowMap>>> {
    tasks::DiarizeWindowTaskInputs inputs;
    inputs.build_session_id = "bs_test";
    inputs.audio = audio_ref(work.wav_path);
    inputs.model_ref = fake_ref(svp::models::kSherpaOnnxDiarizationModelId);
    inputs.work = work;
    inputs.sherpa_library = library;
    std::vector<std::optional<svp::audio::DiarizationWindowMap>> maps(work.window_count);
    for (std::size_t window = work.window_count; window-- > 0;) {
      const svp::exec::TaskSpec spec = tasks::make_diarize_window_task_spec(inputs, window);
      auto [result, payloads] = runtime.run(spec, work.wav_path);
      require(result.status == svp::exec::TaskStatus::succeeded,
              "diarize.window failed: " + (result.error ? result.error->message : std::string()));
      maps[window] = tasks::read_diarize_window_output(spec, result.outputs, payloads);
      require(maps[window].has_value(), "a worker-side window must map or fail the task");
      ++windows_run;
    }
    return maps;
  };
  const svp::audio::SherpaDiarizationResult dispatched =
      svp::audio::run_sherpa_diarization(wav, model_dir, threads, words, {}, dispatch);
  require(windows_run >= 3, "the windows must run as tasks");
  require(dispatched.ran == local.ran, "ran differs");
  require(dispatched.final_speaker_count == local.final_speaker_count, "speaker counts differ");
  require(dispatched.preliminary_cluster_count == local.preliminary_cluster_count,
          "preliminary counts differ");
  require_same_segments(local.preliminary_segments, dispatched.preliminary_segments,
                        "preliminary");
  require_same_segments(local.segments, dispatched.segments, "final");
  require(dispatched.final_speaker_fingerprints == local.final_speaker_fingerprints,
          "fingerprints differ");
  require(dispatched.segment_fingerprint_similarities == local.segment_fingerprint_similarities,
          "similarities differ");
  require(dispatched.word_speaker_assignments == local.word_speaker_assignments,
          "word speakers differ");
  require(dispatched.reconciliation_method == local.reconciliation_method,
          "reconciliation methods differ");
  require(dispatched.blockers == local.blockers, "blockers differ");
  std::filesystem::remove_all(root);
  std::cout << "diarization: " << windows_run << " windows, " << local.segments.size()
            << " segments, " << local.final_speaker_count << " speakers identical\n";
}

}  // namespace

int main() {
  try {
    test_asr_chunk_batch_parameters_round_trip_and_refuse_bad_values();
    test_asr_chunk_batches_cover_every_chunk_and_scale_with_duration();
    test_diarize_window_parameters_round_trip_and_refuse_bad_values();
    test_dispatched_asr_chunks_fold_to_the_local_result();
    test_dispatched_diarization_windows_fold_to_the_local_result();
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << "\n";
    return 1;
  }
  std::cout << "svp-audio-task-tests passed\n";
  return 0;
}
