#include "svp/builder/build_progress.hpp"

#include <stdexcept>

namespace svp::builder {

namespace {

struct StageCatalogEntry {
  ProgressStageId id;
  std::string_view id_string;
  std::string_view label;
};

constexpr StageCatalogEntry stage_catalog[] = {
    {ProgressStageId::media_probe, "media_probe", "Media Probe"},
    {ProgressStageId::media_binding, "media_binding", "Media Binding"},
    {ProgressStageId::audio_extract, "audio_extract", "Audio Extraction"},
    {ProgressStageId::asr, "asr", "ASR"},
    {ProgressStageId::diarization, "diarization", "Diarization"},
    {ProgressStageId::vision_plan, "vision_plan", "Vision Plan"},
    {ProgressStageId::color, "color", "Color Observations"},
    {ProgressStageId::ocr, "ocr", "OCR"},
    {ProgressStageId::ocr_evidence_crops, "ocr_evidence_crops", "OCR Evidence Crops"},
    {ProgressStageId::depth, "depth", "Depth"},
    {ProgressStageId::embeddings, "embeddings", "Embeddings"},
    {ProgressStageId::text_embeddings, "text_embeddings", "Text Embeddings"},
    {ProgressStageId::visual_tracking, "visual_tracking", "Visual Tracking"},
    {ProgressStageId::visual_embeddings, "visual_embeddings", "Visual Embeddings"},
    {ProgressStageId::timeline, "timeline", "Timeline"},
    {ProgressStageId::entities, "entities", "Entities"},
    {ProgressStageId::relationships, "relationships", "Relationships"},
    {ProgressStageId::index, "index", "Index"},
    {ProgressStageId::package_write, "package_write", "Package Write"},
    {ProgressStageId::svpi_write, "svpi_write", "SVPI Write"},
    {ProgressStageId::validate, "validate", "Validation"},
    {ProgressStageId::validation_report, "validation_report", "Validation Report"},
    {ProgressStageId::repackage, "repackage", "Repackage"},
    {ProgressStageId::extract, "extract", "Extract"},
    {ProgressStageId::recombine, "recombine", "Recombine"},
    {ProgressStageId::batch_scan, "batch_scan", "Batch Scan"},
    {ProgressStageId::batch_item, "batch_item", "Batch Item"},
    {ProgressStageId::identity, "identity", "Identity"},
};

constexpr std::size_t stage_catalog_size =
    sizeof(stage_catalog) / sizeof(stage_catalog[0]);

const StageCatalogEntry& catalog_entry(ProgressStageId stage) {
  for (std::size_t i = 0; i < stage_catalog_size; ++i) {
    if (stage_catalog[i].id == stage) {
      return stage_catalog[i];
    }
  }
  throw std::runtime_error("unknown progress stage");
}

}  // namespace

std::string_view progress_stage_id(ProgressStageId stage) {
  return catalog_entry(stage).id_string;
}

std::string_view progress_stage_label(ProgressStageId stage) {
  return catalog_entry(stage).label;
}

std::vector<ProgressStageId> all_progress_stages() {
  std::vector<ProgressStageId> result;
  result.reserve(stage_catalog_size);
  for (std::size_t i = 0; i < stage_catalog_size; ++i) {
    result.push_back(stage_catalog[i].id);
  }
  return result;
}

std::string_view progress_event_kind_name(ProgressEventKind kind) {
  switch (kind) {
    case ProgressEventKind::stage_started:
      return "stage_started";
    case ProgressEventKind::stage_completed:
      return "stage_completed";
    case ProgressEventKind::stage_failed:
      return "stage_failed";
    case ProgressEventKind::stage_progress:
      return "stage_progress";
    case ProgressEventKind::warning:
      return "warning";
    case ProgressEventKind::artifact_written:
      return "artifact_written";
  }
  throw std::runtime_error("unknown progress event kind");
}

void NullBuildProgressSink::emit(const ProgressEvent& /*event*/) {}

std::shared_ptr<BuildProgressSink> default_progress_sink() {
  return std::make_shared<NullBuildProgressSink>();
}

ProgressEvent make_stage_started(ProgressStageId stage, std::string message) {
  return {ProgressEventKind::stage_started, stage, std::move(message), {}};
}

ProgressEvent make_stage_completed(ProgressStageId stage, std::string message) {
  return {ProgressEventKind::stage_completed, stage, std::move(message), {}};
}

ProgressEvent make_stage_failed(ProgressStageId stage, std::string message) {
  return {ProgressEventKind::stage_failed, stage, std::move(message), {}};
}

ProgressEvent make_warning(ProgressStageId stage, std::string message) {
  return {ProgressEventKind::warning, stage, std::move(message), {}};
}

ProgressEvent make_artifact_written(ProgressStageId stage,
                                    std::filesystem::path artifact_path,
                                    std::string message) {
  return {ProgressEventKind::artifact_written, stage, std::move(message),
          std::move(artifact_path)};
}

ProgressEvent make_stage_progress(ProgressStageId stage,
                                  std::uint64_t current,
                                  std::uint64_t total,
                                  std::string unit,
                                  std::string message) {
  ProgressEvent event{ProgressEventKind::stage_progress, stage,
                      std::move(message), {}, current, total, {},
                      std::move(unit)};
  if (total > 0) {
    event.fraction = static_cast<double>(current) / static_cast<double>(total);
  }
  return event;
}

}  // namespace svp::builder
