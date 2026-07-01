#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::builder {

enum class ProgressStageId {
  media_probe,
  media_binding,
  audio_extract,
  asr,
  diarization,
  vision_plan,
  color,
  ocr,
  depth,
  embeddings,
  timeline,
  entities,
  relationships,
  index,
  package_write,
  svpi_write,
  validate,
  extract,
  recombine,
  batch_scan,
  batch_item,
  identity,
};

std::string_view progress_stage_id(ProgressStageId stage);
std::string_view progress_stage_label(ProgressStageId stage);

std::vector<ProgressStageId> all_progress_stages();

enum class ProgressEventKind {
  stage_started,
  stage_completed,
  stage_failed,
  stage_progress,
  warning,
  artifact_written,
};

std::string_view progress_event_kind_name(ProgressEventKind kind);

struct ProgressEvent {
  ProgressEventKind kind;
  ProgressStageId stage_id;
  std::string message;
  std::filesystem::path artifact_path;
  std::optional<std::uint64_t> current;
  std::optional<std::uint64_t> total;
  std::optional<double> fraction;
  std::string unit;
};

class BuildProgressSink {
 public:
  virtual ~BuildProgressSink() = default;
  virtual void emit(const ProgressEvent& event) = 0;
};

class NullBuildProgressSink : public BuildProgressSink {
 public:
  void emit(const ProgressEvent& event) override;
};

std::shared_ptr<BuildProgressSink> default_progress_sink();

ProgressEvent make_stage_started(ProgressStageId stage,
                                 std::string message = "");
ProgressEvent make_stage_completed(ProgressStageId stage,
                                   std::string message = "");
ProgressEvent make_stage_failed(ProgressStageId stage,
                                std::string message = "");
ProgressEvent make_warning(ProgressStageId stage, std::string message);
ProgressEvent make_artifact_written(ProgressStageId stage,
                                    std::filesystem::path artifact_path,
                                    std::string message = "");

ProgressEvent make_stage_progress(ProgressStageId stage,
                                  std::uint64_t current,
                                  std::uint64_t total,
                                  std::string unit,
                                  std::string message = "");

}  // namespace svp::builder
