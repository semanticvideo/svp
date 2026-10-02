#include "svp/builder/video_build_parameters.hpp"

#include "svp/exec/cache_key.hpp"
#include "svp/exec/canonical_json.hpp"
#include "svp/exec/parameters_digest.hpp"
#include "svp/models/thread_plan.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <stdexcept>

namespace svp::builder::batch {
namespace {

constexpr std::array kOcrProfiles = {std::string_view("serial"), std::string_view("background"),
                                     std::string_view("conservative"),
                                     std::string_view("fast")};
constexpr std::array kTrackingQualities = {std::string_view("off"), std::string_view("low"),
                                           std::string_view("medium"),
                                           std::string_view("high")};
constexpr std::array kParameterFields = {
    std::string_view("allow_fallback_diarization"), std::string_view("compute_full_blake3"),
    std::string_view("core_only_diagnostic"),       std::string_view("distributed"),
    std::string_view("ffmpeg_build"),               std::string_view("force_single_speaker"),
    std::string_view("ocr_performance_profile"),    std::string_view("output_format"),
    std::string_view("require_workers"),            std::string_view("run_report"),
    std::string_view("serial_pipeline"),            std::string_view("source_name"),
    std::string_view("thread_plan"),                std::string_view("visual_tracking_quality"),
};

constexpr std::string_view kSourceMediaType = "application/octet-stream";
constexpr std::string_view kModelLockMediaType = "application/json";

[[noreturn]] void invalid(const std::string& field, const std::string& why) {
  throw std::invalid_argument("video.build parameters: " + field + " " + why);
}

const nlohmann::json& field(const nlohmann::json& value, std::string_view name) {
  const auto found = value.find(std::string(name));
  if (found == value.end()) {
    invalid(std::string(name), "is missing");
  }
  return *found;
}

bool boolean(const nlohmann::json& value, std::string_view name) {
  const nlohmann::json& member = field(value, name);
  if (!member.is_boolean()) {
    invalid(std::string(name), "must be a boolean");
  }
  return member.get<bool>();
}

std::string text(const nlohmann::json& value, std::string_view name) {
  const nlohmann::json& member = field(value, name);
  if (!member.is_string()) {
    invalid(std::string(name), "must be a string");
  }
  return member.get<std::string>();
}

template <std::size_t N>
std::string one_of(const nlohmann::json& value, std::string_view name,
                   const std::array<std::string_view, N>& allowed) {
  std::string chosen = text(value, name);
  if (std::find(allowed.begin(), allowed.end(), chosen) == allowed.end()) {
    invalid(std::string(name), "is not one of the accepted values: `" + chosen + "`");
  }
  return chosen;
}

}  // namespace

std::string_view video_output_format_name(VideoOutputFormat format) noexcept {
  switch (format) {
    case VideoOutputFormat::svp:
      return "svp";
    case VideoOutputFormat::svpi:
      return "svpi";
    case VideoOutputFormat::embedded_svpi:
      return "embedded-svpi";
  }
  return "svp";
}

std::optional<VideoOutputFormat> parse_video_output_format(std::string_view name) noexcept {
  for (const VideoOutputFormat format :
       {VideoOutputFormat::svp, VideoOutputFormat::svpi, VideoOutputFormat::embedded_svpi}) {
    if (video_output_format_name(format) == name) {
      return format;
    }
  }
  return std::nullopt;
}

bool is_plain_file_name(std::string_view name) noexcept {
  return !name.empty() && name.size() <= NAME_MAX && name != "." && name != ".." &&
         name.find('/') == std::string_view::npos && name.find('\0') == std::string_view::npos;
}

bool operator==(const VideoBuildParameters& left, const VideoBuildParameters& right) {
  return video_build_parameters_to_json(left) == video_build_parameters_to_json(right);
}

nlohmann::json video_build_parameters_to_json(const VideoBuildParameters& parameters) {
  return nlohmann::json{
      {"allow_fallback_diarization", parameters.allow_fallback_diarization},
      {"compute_full_blake3", parameters.compute_full_blake3},
      {"core_only_diagnostic", parameters.core_only_diagnostic},
      {"distributed", parameters.distributed},
      {"ffmpeg_build", parameters.ffmpeg_build},
      {"force_single_speaker", parameters.force_single_speaker},
      {"ocr_performance_profile", parameters.performance.ocr_performance_profile},
      {"output_format", std::string(video_output_format_name(parameters.output_format))},
      {"require_workers", parameters.require_workers},
      {"run_report", parameters.run_report},
      {"serial_pipeline", parameters.serial_pipeline},
      {"source_name", parameters.source_name},
      {"thread_plan", parameters.thread_plan},
      {"visual_tracking_quality", parameters.visual_tracking_quality},
  };
}

VideoBuildParameters video_build_parameters_from_json(const nlohmann::json& value) {
  if (!value.is_object()) {
    invalid("", "must be an object");
  }
  for (const auto& [name, member] : value.items()) {
    if (std::find(kParameterFields.begin(), kParameterFields.end(), name) ==
        kParameterFields.end()) {
      invalid(name, "is not a video.build parameter");
    }
  }
  VideoBuildParameters parameters;
  const std::string format = text(value, "output_format");
  const std::optional<VideoOutputFormat> parsed = parse_video_output_format(format);
  if (!parsed) {
    invalid("output_format", "is not svp, svpi, or embedded-svpi: `" + format + "`");
  }
  parameters.output_format = *parsed;
  parameters.source_name = text(value, "source_name");
  if (!is_plain_file_name(parameters.source_name)) {
    invalid("source_name", "must be a plain file name");
  }
  parameters.performance.ocr_performance_profile =
      one_of(value, "ocr_performance_profile", kOcrProfiles);
  parameters.visual_tracking_quality =
      one_of(value, "visual_tracking_quality", kTrackingQualities);
  parameters.allow_fallback_diarization = boolean(value, "allow_fallback_diarization");
  parameters.force_single_speaker = boolean(value, "force_single_speaker");
  parameters.serial_pipeline = boolean(value, "serial_pipeline");
  parameters.compute_full_blake3 = boolean(value, "compute_full_blake3");
  parameters.core_only_diagnostic = boolean(value, "core_only_diagnostic");
  parameters.distributed = boolean(value, "distributed");
  parameters.run_report = boolean(value, "run_report");
  const nlohmann::json& require_workers = field(value, "require_workers");
  if (!require_workers.is_number_unsigned()) {
    invalid("require_workers", "must be an unsigned integer");
  }
  parameters.require_workers = require_workers.get<std::uint64_t>();
  parameters.ffmpeg_build = text(value, "ffmpeg_build");
  if (!parameters.ffmpeg_build.empty() &&
      !svp::exec::parse_blake3_prefixed(parameters.ffmpeg_build)) {
    invalid("ffmpeg_build", "must be b3:<hex> or empty");
  }
  parameters.thread_plan = field(value, "thread_plan");
  std::optional<svp::models::ThreadPlan> plan;
  try {
    plan = svp::models::thread_plan_from_json(parameters.thread_plan);
  } catch (const std::exception& error) {
    invalid("thread_plan", std::string("is not a thread plan: ") + error.what());
  }
  if (!svp::models::thread_plan_problems(*plan).empty() ||
      !svp::models::thread_plan_is_host_independent(*plan)) {
    invalid("thread_plan", "must fix every thread count");
  }
  return parameters;
}

std::optional<std::string> validate_video_build_parameters(const nlohmann::json& value) {
  try {
    (void)video_build_parameters_from_json(value);
    return std::nullopt;
  } catch (const std::exception& error) {
    return std::string(error.what());
  }
}

std::string encode_video_build_package_record(const VideoBuildPackageRecord& record) {
  return svp::exec::encode_canonical_json(nlohmann::json{
      {"blob", nlohmann::json{{"blake3", svp::exec::blake3_hex(record.blake3)},
                              {"bytes", record.bytes}}},
      {"file_name", record.file_name}});
}

VideoBuildPackageRecord decode_video_build_package_record(std::string_view bytes) {
  const nlohmann::json value = nlohmann::json::parse(bytes, nullptr, false);
  const auto fail = [](const std::string& why) {
    throw std::invalid_argument("video.build package record: " + why);
  };
  if (!value.is_object() || value.size() != 2 || !value.contains("blob") ||
      !value.contains("file_name") || !value.at("file_name").is_string()) {
    fail("needs exactly blob and file_name");
  }
  const nlohmann::json& blob = value.at("blob");
  if (!blob.is_object() || blob.size() != 2 || !blob.contains("blake3") ||
      !blob.contains("bytes") || !blob.at("blake3").is_string() ||
      !blob.at("bytes").is_number_unsigned()) {
    fail("blob needs exactly blake3 and bytes");
  }
  const std::optional<svp::exec::Blake3Digest> digest =
      svp::exec::parse_blake3_hex(blob.at("blake3").get<std::string>());
  if (!digest) {
    fail("blob.blake3 is not a BLAKE3 digest");
  }
  VideoBuildPackageRecord record{.file_name = value.at("file_name").get<std::string>(),
                                 .blake3 = *digest,
                                 .bytes = blob.at("bytes").get<std::uint64_t>()};
  if (!is_plain_file_name(record.file_name)) {
    fail("file_name must be a plain file name");
  }
  return record;
}

svp::exec::TaskSpec make_video_build_spec(const VideoBuildSpecInput& input) {
  svp::exec::TaskSpec spec;
  spec.build_session_id = input.build_session_id;
  spec.task_id = input.task_id;
  spec.task_type = std::string(kVideoBuildTaskType);
  spec.task_type_version = kVideoBuildTaskTypeVersion;
  spec.model_refs = input.model_refs;
  std::sort(spec.model_refs.begin(), spec.model_refs.end(),
            [](const auto& left, const auto& right) { return left.model_id < right.model_id; });
  svp::exec::ArtifactRef source = input.source;
  source.media_type = std::string(kSourceMediaType);
  source.role = std::string(kVideoBuildSourceInput);
  svp::exec::ArtifactRef model_lock = input.model_lock;
  model_lock.media_type = std::string(kModelLockMediaType);
  model_lock.role = std::string(kVideoBuildModelLockInput);
  spec.inputs.emplace(std::string(kVideoBuildSourceInput), source);
  spec.inputs.emplace(std::string(kVideoBuildModelLockInput), model_lock);
  spec.parameters = video_build_parameters_to_json(input.parameters);
  spec.parameters_blake3 = svp::exec::compute_parameters_blake3(spec.parameters);
  nlohmann::json bundle_ids = nlohmann::json::array();
  for (const svp::exec::TaskModelRef& ref : spec.model_refs) {
    bundle_ids.push_back(ref.model_bundle_id);
  }
  spec.cache_key = svp::exec::compute_cache_key(nlohmann::json::array({
      spec.task_type,
      spec.task_type_version,
      svp::exec::blake3_hex(source.blake3),
      std::move(bundle_ids),
      svp::exec::blake3_hex(spec.parameters_blake3),
  }));
  spec.resources = svp::exec::TaskResources{.est_peak_rss_mb = kVideoBuildEstimatedPeakRssMb,
                                            .est_cpu_threads = kVideoBuildEstimatedCpuThreads,
                                            .est_seconds = 0};
  svp::exec::validate_task_spec(spec);
  return spec;
}

}  // namespace svp::builder::batch
