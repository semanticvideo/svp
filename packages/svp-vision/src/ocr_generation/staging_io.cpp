#include "ocr_generation_internal.hpp"

#include "svp/vision/evidence_crop.hpp"

#include <filesystem>
#include <fstream>

namespace svp::vision::ocr_generation_internal {

void write_failure_stage_files(
    const std::filesystem::path& staging_dir,
    const TextAbsenceRecord& text_absence) {
  const std::filesystem::path text_dir = staging_dir / "text";
  std::filesystem::create_directories(text_dir);
  {
    std::ofstream out(text_dir / "text_regions.jsonl");
  }
  {
    std::ofstream out(text_dir / "text_observations.jsonl");
  }
  {
    std::ofstream out(text_dir / "numeric_values.jsonl");
  }
  {
    std::ofstream out(text_dir / "evidence_crops.jsonl");
  }
  {
    std::ofstream out(text_dir / "text_absence.json");
    if (out) {
      out << text_absence_to_json(text_absence).dump(2) << "\n";
    }
  }
}

bool write_success_stage_files(
    const std::filesystem::path& staging_dir,
    OcrGenerationResult& result) {
  const std::filesystem::path text_dir = staging_dir / "text";
  std::filesystem::create_directories(text_dir);

  {
    std::ofstream out(text_dir / "text_regions.jsonl");
    if (!out) {
      result.blocker = "Failed to open text_regions.jsonl";
      return false;
    }
    for (const auto& reg : result.text_regions) {
      out << text_region_to_json(reg).dump() << "\n";
    }
  }
  result.text_regions_written = true;

  {
    std::ofstream out(text_dir / "text_observations.jsonl");
    if (!out) {
      result.blocker = "Failed to open text_observations.jsonl";
      return false;
    }
    for (const auto& obs : result.text_observations) {
      out << text_observation_to_json(obs).dump() << "\n";
    }
  }
  result.text_observations_written = true;

  {
    std::ofstream out(text_dir / "numeric_values.jsonl");
    if (!out) {
      result.blocker = "Failed to open numeric_values.jsonl";
      return false;
    }
    for (const auto& nv : result.numeric_values) {
      out << numeric_value_to_json(nv).dump() << "\n";
    }
  }
  result.numeric_values_written = true;

  if (!result.evidence_crops_written) {
    std::ofstream out(text_dir / "evidence_crops.jsonl");
    result.evidence_crops_written = true;
  }

  {
    std::ofstream out(text_dir / "text_absence.json");
    if (!out) {
      result.blocker = "Failed to open text_absence.json";
      return false;
    }
    out << text_absence_to_json(result.text_absence).dump(2) << "\n";
  }
  result.text_absence_written = true;
  return true;
}

}  // namespace svp::vision::ocr_generation_internal
