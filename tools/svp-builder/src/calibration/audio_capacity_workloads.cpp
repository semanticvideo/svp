#include "calibration/audio_capacity_workloads.hpp"

#include "svp/audio/asr_chunk_planner.hpp"
#include "svp/audio/diarization_window_map.hpp"
#include "svp/audio/tasks/asr_chunk_batch.hpp"
#include "svp/audio/tasks/audio_task_environment.hpp"
#include "svp/audio/tasks/diarize_window.hpp"
#include "svp/exec/blake3_digest.hpp"
#include "svp/models/reference_processor_model_ids.hpp"

#include <cstdlib>
#include <stdexcept>
#include <sys/wait.h>

namespace svp::builder::calibration {
namespace {

namespace audio_tasks = svp::audio::tasks;

constexpr const char* kCalibrationSession = "bs_audio_calibration";
// Calibration specs carry the minimum estimate: their leases come from the
// lease policy's floors (30 s, renewed by heartbeats; 10 min hard deadline),
// which bound one calibration batch on any Mac.
constexpr std::uint64_t kCalibrationEstimatedSeconds = 1;
// The analysis audio format the clip is converted to (and every audio task
// reads): 16 kHz mono.
constexpr std::int64_t kAnalysisSampleRate = 16000;
constexpr std::int64_t kMicrosecondsPerSecond = 1000000;

// About half a minute of plain English when spoken at the synthesizer's
// default rate: several ASR chunks, one diarization window.
constexpr const char* kCalibrationPassage =
    "This recording measures how quickly a computer can turn speech into text. "
    "It reads a short passage at an even pace, with ordinary words and a few numbers, "
    "such as twelve, forty seven, and three hundred. "
    "Each sentence is long enough to give the recognizer real work, "
    "and the pauses between them are short. "
    "The same words are read every time, so every machine hears exactly the same sound. "
    "When the passage ends, the measurement is complete, "
    "and the result decides how much work each machine is given.";

std::string shell_quoted(const std::string& text) {
  std::string quoted = "'";
  for (const char character : text) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted += character;
    }
  }
  return quoted + "'";
}

void run(const std::string& command, const std::string& what) {
  const int status = std::system(command.c_str());
  if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    throw std::runtime_error("could not " + what + " for the speech calibration clip");
  }
}

std::size_t clip_samples(const std::filesystem::path& clip_file) {
  const std::size_t samples = svp::audio::diarization_wav_sample_count(clip_file);
  if (samples == 0) {
    throw std::runtime_error("the speech calibration clip has no samples");
  }
  return samples;
}

svp::exec::TaskSpec with_minimum_estimate(svp::exec::TaskSpec spec) {
  spec.resources.est_seconds = kCalibrationEstimatedSeconds;
  return spec;
}

svp::audio::WhisperRuntimeThreads asr_threads(const svp::models::ThreadPlan& plan) {
  return {.whisper = plan.whisper, .forced_alignment = plan.forced_alignment};
}

}  // namespace

std::filesystem::path write_speech_calibration_clip(const std::filesystem::path& ffmpeg,
                                                    const std::filesystem::path& directory) {
  std::filesystem::create_directories(directory);
  const std::filesystem::path spoken = directory / "speech-calibration.aiff";
  const std::filesystem::path clip = directory / "speech-calibration.wav";
  run("/usr/bin/say -o " + shell_quoted(spoken.string()) + " " +
          shell_quoted(kCalibrationPassage),
      "render speech with /usr/bin/say");
  run(shell_quoted(ffmpeg.string()) + " -v error -nostdin -y -i " +
          shell_quoted(spoken.string()) +
          " -map_metadata -1 -ac 1 -ar " + std::to_string(kAnalysisSampleRate) +
          " -c:a pcm_s16le -fflags +bitexact -flags:a +bitexact " +
          shell_quoted(clip.string()),
      "convert speech with ffmpeg");
  std::error_code error;
  std::filesystem::remove(spoken, error);
  return clip;
}

std::vector<std::string> audio_task_types(const DistributedAudioWork& audio) {
  std::vector<std::string> types;
  if (!audio.asr_model_refs.empty()) {
    types.emplace_back(audio_tasks::kAsrChunkBatchTaskType);
  }
  if (audio.diarization_model_ref) {
    types.emplace_back(audio_tasks::kDiarizeWindowTaskType);
  }
  return types;
}

std::uint64_t audio_task_peak_rss_mb(std::string_view task_type) {
  if (task_type == audio_tasks::kAsrChunkBatchTaskType) {
    return audio_tasks::kAsrChunkBatchEstimatedPeakRssMb;
  }
  if (task_type == audio_tasks::kDiarizeWindowTaskType) {
    return audio_tasks::kDiarizeWindowEstimatedPeakRssMb;
  }
  throw std::runtime_error("not an audio task type: " + std::string(task_type));
}

nlohmann::json audio_capacity_settings(std::string_view task_type,
                                       const AudioCalibrationSetup& setup,
                                       const svp::exec::ArtifactRef& clip) {
  nlohmann::json settings = {{"clip", svp::exec::blake3_hex(clip.blake3)},
                             {"recipe", kAudioCalibrationRecipeVersion},
                             {"task_type", std::string(task_type)}};
  if (task_type == audio_tasks::kAsrChunkBatchTaskType) {
    nlohmann::json bundles = nlohmann::json::array();
    for (const svp::exec::TaskModelRef& ref : setup.audio.asr_model_refs) {
      bundles.push_back(ref.model_bundle_id);
    }
    settings["models"] = std::move(bundles);
    settings["threads"] = {{"alignment_inter_op", setup.thread_plan.forced_alignment.inter_op},
                           {"alignment_intra_op", setup.thread_plan.forced_alignment.intra_op},
                           {"decode", setup.thread_plan.whisper.decode},
                           {"vad", setup.thread_plan.whisper.vad}};
  } else if (task_type == audio_tasks::kDiarizeWindowTaskType) {
    if (!setup.audio.diarization_model_ref) {
      throw std::runtime_error("this build does not dispatch diarize.window");
    }
    settings["models"] = {setup.audio.diarization_model_ref->model_bundle_id};
    settings["sherpa_library"] = setup.sherpa_library;
    settings["threads"] = {{"embedding", setup.thread_plan.sherpa.embedding},
                           {"segmentation", setup.thread_plan.sherpa.segmentation}};
  } else {
    throw std::runtime_error("not an audio task type: " + std::string(task_type));
  }
  return settings;
}

CapacityWorkload audio_capacity_workload(std::string_view task_type,
                                         const AudioCalibrationSetup& setup,
                                         const svp::exec::ArtifactRef& clip,
                                         const std::filesystem::path& clip_file) {
  const std::size_t samples = clip_samples(clip_file);
  if (task_type == audio_tasks::kAsrChunkBatchTaskType) {
    if (setup.audio.asr_model_refs.empty()) {
      throw std::runtime_error("this build does not dispatch asr.chunk_batch");
    }
    const std::int64_t duration_us =
        static_cast<std::int64_t>(samples) * kMicrosecondsPerSecond / kAnalysisSampleRate;
    audio_tasks::AsrChunkBatchTaskInputs inputs;
    inputs.build_session_id = kCalibrationSession;
    inputs.audio = clip;
    inputs.model_refs = setup.audio.asr_model_refs;
    inputs.work.chunks = svp::audio::build_asr_chunk_plan(duration_us).chunks;
    inputs.work.input_wav = clip_file;
    for (const svp::exec::TaskModelRef& ref : setup.audio.asr_model_refs) {
      if (ref.model_id == svp::models::kWhisperSmallEnglishModelId) {
        inputs.work.model_id = ref.model_id;
      } else if (ref.model_id == svp::models::kWhisperCppSileroVadModelId) {
        inputs.work.vad_model_id = ref.model_id;
      } else {
        inputs.work.alignment_model_id = ref.model_id;
      }
    }
    inputs.work.threads = asr_threads(setup.thread_plan);
    inputs.batch_policy.estimated_seconds_per_item = inputs.batch_policy.target_task_seconds;
    if (inputs.work.chunks.empty()) {
      throw std::runtime_error("the speech calibration clip has no ASR chunks");
    }
    return CapacityWorkload{
        .name = std::string(task_type),
        .batch = with_minimum_estimate(audio_tasks::make_asr_chunk_batch_task_spec(
            inputs, {.first = 0, .count = inputs.work.chunks.size()})),
        .warm_up = with_minimum_estimate(
            audio_tasks::make_asr_chunk_batch_task_spec(inputs, {.first = 0, .count = 1})),
        .items_per_batch = inputs.work.chunks.size()};
  }
  if (task_type == audio_tasks::kDiarizeWindowTaskType) {
    if (!setup.audio.diarization_model_ref) {
      throw std::runtime_error("this build does not dispatch diarize.window");
    }
    audio_tasks::DiarizeWindowTaskInputs inputs;
    inputs.build_session_id = kCalibrationSession;
    inputs.audio = clip;
    inputs.model_ref = *setup.audio.diarization_model_ref;
    inputs.work.wav_path = clip_file;
    inputs.work.sample_count = samples;
    inputs.work.window_count = svp::audio::diarization_window_count(samples);
    inputs.work.settings = {.threads = setup.thread_plan.sherpa, .compute_embeddings = true};
    inputs.sherpa_library = setup.sherpa_library;
    const std::uint64_t whole_seconds =
        static_cast<std::uint64_t>(svp::audio::diarization_window_samples(samples, 0)) /
        static_cast<std::uint64_t>(kAnalysisSampleRate);
    if (whole_seconds == 0) {
      throw std::runtime_error("the speech calibration clip is shorter than a second");
    }
    const svp::exec::TaskSpec window = with_minimum_estimate(
        audio_tasks::make_diarize_window_task_spec(inputs, 0));
    return CapacityWorkload{.name = std::string(task_type),
                            .batch = window,
                            .warm_up = window,
                            .items_per_batch = whole_seconds};
  }
  throw std::runtime_error("not an audio task type: " + std::string(task_type));
}

}  // namespace svp::builder::calibration
