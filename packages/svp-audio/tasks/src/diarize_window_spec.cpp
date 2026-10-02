#include "svp/audio/tasks/diarize_window.hpp"

#include "audio_task_spec.hpp"
#include "json_fields.hpp"
#include "task_outputs.hpp"
#include "window_map_codec.hpp"

#include "svp/audio/sherpa_diarization.hpp"
#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/source_fingerprint.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <stdexcept>

namespace svp::audio::tasks {
namespace {

// Samples per second of the analysis WAV diarization reads (16 kHz mono,
// the same rate run_sherpa_diarization windows by).
constexpr double kAnalysisSampleRate = 16000.0;

nlohmann::json floats_json(const std::vector<float>& values) {
  nlohmann::json array = nlohmann::json::array();
  for (const float value : values) {
    array.push_back(value);
  }
  return array;
}

std::vector<float> floats_from_json(const nlohmann::json& value, const std::string& where) {
  if (!value.is_array()) {
    throw std::invalid_argument(where + " must be an array of numbers");
  }
  std::vector<float> values;
  values.reserve(value.size());
  for (const nlohmann::json& item : value) {
    if (!item.is_number()) {
      throw std::invalid_argument(where + " must be an array of numbers");
    }
    values.push_back(item.get<float>());
  }
  return values;
}

}  // namespace

nlohmann::json diarize_window_parameters_to_json(const DiarizeWindowParameters& parameters) {
  nlohmann::json value = {
      {"compute_embeddings", parameters.compute_embeddings},
      {"model_id", parameters.model_id},
      {"sample_count", parameters.sample_count},
      {"sherpa_library", parameters.sherpa_library},
      {"threads",
       {{"embedding", parameters.threads.embedding},
        {"segmentation", parameters.threads.segmentation}}},
      {"window_index", parameters.window_index},
  };
  if (const std::optional<std::string> problem = validate_diarize_window_parameters(value)) {
    throw std::invalid_argument("diarize.window parameters: " + *problem);
  }
  return value;
}

DiarizeWindowParameters diarize_window_parameters_from_json(const nlohmann::json& value) {
  const std::string where = "diarize.window parameters";
  detail::require_only(value,
                       {"compute_embeddings", "model_id", "sample_count", "sherpa_library",
                        "threads", "window_index"},
                       where);
  DiarizeWindowParameters parameters;
  parameters.compute_embeddings = detail::required<bool>(value, "compute_embeddings", where);
  parameters.model_id = detail::required<std::string>(value, "model_id", where);
  parameters.sample_count = detail::required<std::uint64_t>(value, "sample_count", where);
  parameters.sherpa_library = detail::required<std::string>(value, "sherpa_library", where);
  parameters.window_index = detail::required<std::uint64_t>(value, "window_index", where);
  const nlohmann::json& threads = value.at("threads");
  detail::require_only(threads, {"embedding", "segmentation"}, where + ".threads");
  parameters.threads.embedding = detail::required<int>(threads, "embedding", where);
  parameters.threads.segmentation = detail::required<int>(threads, "segmentation", where);
  if (parameters.threads.embedding < 1 || parameters.threads.segmentation < 1) {
    throw std::invalid_argument(where + ": thread counts must be at least 1");
  }
  if (parameters.model_id.empty()) {
    throw std::invalid_argument(where + ": model_id must not be empty");
  }
  if (!svp::exec::parse_blake3_prefixed(parameters.sherpa_library)) {
    throw std::invalid_argument(where + ": sherpa_library must be b3:<hex>");
  }
  if (parameters.sample_count == 0 ||
      parameters.window_index >=
          svp::audio::diarization_window_count(static_cast<std::size_t>(parameters.sample_count))) {
    throw std::invalid_argument(where + ": window_index is outside the WAV's windows");
  }
  return parameters;
}

std::optional<std::string> validate_diarize_window_parameters(const nlohmann::json& value) {
  try {
    (void)diarize_window_parameters_from_json(value);
    return std::nullopt;
  } catch (const std::exception& error) {
    return std::string(error.what());
  }
}

svp::exec::TaskOrderKey diarize_window_order_key(std::uint64_t run_ordinal,
                                                 std::size_t window_index) {
  return svp::exec::TaskOrderKey{.lane = std::string(kDiarizeWindowLane),
                                 .ordinals = {run_ordinal, window_index}};
}

svp::exec::TaskSpec make_diarize_window_task_spec(const DiarizeWindowTaskInputs& inputs,
                                                  std::size_t window_index) {
  if (window_index >= inputs.work.window_count) {
    throw std::invalid_argument("diarize.window window is outside the work");
  }
  if (!(inputs.seconds_per_audio_second > 0.0) ||
      !std::isfinite(inputs.seconds_per_audio_second)) {
    throw std::invalid_argument("diarize.window cost needs positive seconds per audio second");
  }
  DiarizeWindowParameters parameters;
  parameters.window_index = window_index;
  parameters.sample_count = inputs.work.sample_count;
  parameters.compute_embeddings = inputs.work.settings.compute_embeddings;
  parameters.threads = inputs.work.settings.threads;
  parameters.model_id = inputs.model_ref.model_id;
  parameters.sherpa_library = inputs.sherpa_library;
  const double audio_seconds =
      static_cast<double>(svp::audio::diarization_window_samples(inputs.work.sample_count,
                                                                 window_index)) /
      kAnalysisSampleRate;
  return detail::assemble_audio_task_spec(detail::AudioSpecAssembly{
      .build_session_id = inputs.build_session_id,
      .task_type = std::string(kDiarizeWindowTaskType),
      .task_type_version = kDiarizeWindowTaskTypeVersion,
      .task_id = detail::audio_task_id(kDiarizeWindowTaskType, inputs.run_ordinal, window_index,
                                       window_index),
      .model_refs = {inputs.model_ref},
      .audio = inputs.audio,
      .parameters = diarize_window_parameters_to_json(parameters),
      .resources = svp::exec::TaskResources{
          .est_peak_rss_mb = kDiarizeWindowEstimatedPeakRssMb,
          .est_cpu_threads = static_cast<std::uint64_t>(std::max(
              {1, parameters.threads.segmentation, parameters.threads.embedding})),
          .est_seconds = std::max<std::uint64_t>(
              1, static_cast<std::uint64_t>(
                     std::ceil(audio_seconds * inputs.seconds_per_audio_second)))},
  });
}

std::optional<svp::audio::DiarizationWindowMap> read_diarize_window_output(
    const svp::exec::TaskSpec& spec, const std::vector<svp::exec::ArtifactRef>& outputs,
    const std::vector<std::vector<std::byte>>& payloads) {
  const DiarizeWindowParameters parameters = diarize_window_parameters_from_json(spec.parameters);
  const nlohmann::json body =
      detail::read_cbor_output(spec, outputs, payloads, kDiarizeWindowMapRole);
  if (body.at("window_index").get<std::uint64_t>() != parameters.window_index) {
    throw std::invalid_argument(spec.task_id + ": output is not window " +
                                std::to_string(parameters.window_index));
  }
  if (body.contains("not_mapped")) {
    return std::nullopt;
  }
  svp::audio::DiarizationWindowMap map;
  map.window_index = parameters.window_index;
  const nlohmann::json& pieces = body.at("pieces");
  const std::size_t expected_pieces = svp::audio::diarization_window_piece_count(
      static_cast<std::size_t>(parameters.sample_count), map.window_index);
  if (!pieces.is_array() || (!pieces.empty() && pieces.size() != expected_pieces)) {
    throw std::invalid_argument(spec.task_id + ": output does not hold the window's pieces");
  }
  for (std::size_t piece_index = 0; piece_index < pieces.size(); ++piece_index) {
    svp::audio::DiarizationWindowPiece piece;
    for (const nlohmann::json& record : pieces[piece_index]) {
      const std::string where =
          spec.task_id + " piece " + std::to_string(piece_index) + " speaker";
      svp::audio::DiarizationPieceSpeaker speaker;
      speaker.local_speaker = record.at("local_speaker").get<int32_t>();
      speaker.window_observation = record.at("observation").get<std::size_t>();
      speaker.starts_observation = record.at("starts").get<bool>();
      speaker.embedding = floats_from_json(record.at("embedding"), where + " embedding");
      const std::vector<float> bounds = floats_from_json(record.at("segments"), where);
      if (bounds.empty() || bounds.size() % 2 != 0) {
        throw std::invalid_argument(where + " has malformed segments");
      }
      for (std::size_t index = 0; index < bounds.size(); index += 2) {
        speaker.segments.push_back({.start_sec = bounds[index],
                                    .end_sec = bounds[index + 1],
                                    .speaker_id = speaker.local_speaker});
      }
      piece.speakers.push_back(std::move(speaker));
    }
    map.pieces.push_back(std::move(piece));
  }
  return map;
}

nlohmann::json diarize_window_map_json(const svp::audio::DiarizationWindowMap& map) {
  nlohmann::json pieces = nlohmann::json::array();
  for (const svp::audio::DiarizationWindowPiece& piece : map.pieces) {
    nlohmann::json speakers = nlohmann::json::array();
    for (const svp::audio::DiarizationPieceSpeaker& speaker : piece.speakers) {
      std::vector<float> bounds;
      for (const svp::audio::SherpaDiarizationSegment& segment : speaker.segments) {
        bounds.push_back(segment.start_sec);
        bounds.push_back(segment.end_sec);
      }
      speakers.push_back({{"embedding", floats_json(speaker.embedding)},
                          {"local_speaker", speaker.local_speaker},
                          {"observation", speaker.window_observation},
                          {"segments", floats_json(bounds)},
                          {"starts", speaker.starts_observation}});
    }
    pieces.push_back(std::move(speakers));
  }
  return {{"pieces", std::move(pieces)}, {"window_index", map.window_index}};
}

std::optional<std::string> loaded_sherpa_library_identity() {
  static std::mutex mutex;
  const std::scoped_lock lock(mutex);
  if (!svp::audio::is_sherpa_diarization_available()) {
    return std::nullopt;
  }
  const std::string path = svp::audio::sherpa_lib_path_used();
  if (path.empty()) {
    return std::nullopt;
  }
  try {
    return svp::exec::blake3_prefixed(svp::exec::fingerprint_source("sherpa", path).blake3);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::optional<std::string> expected_sherpa_library_identity() {
  const std::string path = svp::audio::sherpa_lib_path_expected();
  if (path.empty()) {
    return std::nullopt;
  }
  try {
    return svp::exec::blake3_prefixed(svp::exec::fingerprint_source("sherpa", path).blake3);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

}  // namespace svp::audio::tasks
