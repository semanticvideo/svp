#include "svp/builder/interlace.hpp"
#include "svp/builder/build_progress.hpp"

#include "svp/package/media_binding.hpp"
#include "svp/package/media_binding_factory.hpp"
#include "svp/package/package_layout.hpp"
#include "svp/package/svpi_writer.hpp"
#include "svp/package/index_writer.hpp"
#include "svp/validation/validator.hpp"
#include "svp/validation/svpi_validator.hpp"

#include <nlohmann/json.hpp>
#include <zip.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>

namespace svp::builder {

namespace {

struct ZipDeleter {
  void operator()(zip_t* archive) const noexcept {
    if (archive != nullptr) {
      zip_discard(archive);
    }
  }
};

using ZipArchive = std::unique_ptr<zip_t, ZipDeleter>;

std::string make_utc_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto time_t_now = std::chrono::system_clock::to_time_t(now);
  std::ostringstream ss;
  ss << std::put_time(std::gmtime(&time_t_now), "%Y-%m-%dT%H:%M:%SZ");
  return ss.str();
}

void write_jsonl(const std::filesystem::path& path,
                 const std::vector<nlohmann::json>& records) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  for (const auto& record : records) {
    out << record.dump() << "\n";
  }
}

bool extract_zip_entry(zip_t* archive, const std::string& entry_name,
                       const std::filesystem::path& output_path) {
  zip_file_t* file = zip_fopen(archive, entry_name.c_str(), 0);
  if (!file) {
    return false;
  }

  std::filesystem::create_directories(output_path.parent_path());
  std::ofstream out(output_path, std::ios::binary);
  if (!out) {
    zip_fclose(file);
    return false;
  }

  std::array<char, 64 * 1024> buffer{};
  zip_int64_t count;
  while ((count = zip_fread(file, buffer.data(), buffer.size())) > 0) {
    out.write(buffer.data(), static_cast<std::streamsize>(count));
  }

  zip_fclose(file);
  return out.good();
}

std::vector<std::string> find_media_original_entries(zip_t* archive) {
  std::vector<std::string> entries;
  const auto num = zip_get_num_entries(archive, 0);
  for (zip_int64_t i = 0; i < num; ++i) {
    zip_stat_t stat;
    zip_stat_init(&stat);
    if (zip_stat_index(archive, static_cast<zip_uint64_t>(i), 0, &stat) != 0) {
      continue;
    }
    if (stat.name == nullptr) {
      continue;
    }
    std::string name(stat.name);
    if (name.rfind("media/original/", 0) == 0 && name.back() != '/') {
      entries.push_back(name);
    }
  }
  return entries;
}

}  // namespace

InterlaceExtractResult interlace_extract(const InterlaceExtractOptions& options) {
  InterlaceExtractResult result;

  std::shared_ptr<BuildProgressSink> sink = options.progress_sink;
  if (!sink) {
    sink = default_progress_sink();
  }

  sink->emit(make_stage_started(ProgressStageId::extract));

  const std::filesystem::path svp_path(options.svp_path);
  if (!std::filesystem::exists(svp_path)) {
    result.error_message = "SVP file does not exist: " + options.svp_path;
    return result;
  }

  const std::filesystem::path out_dir(options.out_dir);
  std::filesystem::create_directories(out_dir);

  int zip_error = ZIP_ER_OK;
  ZipArchive archive{zip_open(svp_path.string().c_str(), ZIP_RDONLY, &zip_error)};
  if (!archive) {
    result.error_message = "could not open SVP as ZIP archive";
    return result;
  }

  auto media_entries = find_media_original_entries(archive.get());
  if (media_entries.empty()) {
    result.error_message = "no media/original/ entries found in SVP";
    return result;
  }

  const auto& media_entry = media_entries[0];
  std::string media_filename =
      std::filesystem::path(media_entry).filename().string();
  result.media_filename = media_filename;
  result.extracted_media_path = out_dir / media_filename;

  if (!extract_zip_entry(archive.get(), media_entry, result.extracted_media_path)) {
    result.error_message = "failed to extract media: " + media_entry;
    return result;
  }

  std::string svpi_filename =
      std::filesystem::path(svp_path).stem().string() + ".svpi";
  result.extracted_svpi_path = out_dir / svpi_filename;

  auto manifest_entry = svp::package::read_package_entry(svp_path, "manifest.json");
  if (!manifest_entry.has_value()) {
    result.error_message = "could not read manifest.json from SVP";
    return result;
  }

  auto svp_manifest = nlohmann::json::parse(manifest_entry.value(), nullptr, false);
  if (svp_manifest.is_discarded() || !svp_manifest.is_object()) {
    result.error_message = "manifest.json is not valid JSON";
    return result;
  }

  svp::package::MediaBindingFactoryOptions binding_opts;
  binding_opts.ffprobe_path = options.ffprobe_path;
  binding_opts.compute_full_blake3 = true;
  binding_opts.compute_chunk_proof = true;
  binding_opts.binding_id = "mb_primary_000001";
  binding_opts.media_id =
      svp_manifest.value("primary_media_id", "media_000001");

  auto binding_doc = svp::package::create_media_binding(
      result.extracted_media_path, binding_opts);

  nlohmann::json svpi_manifest = {
    {"format", "svpi"},
    {"svpi_version", std::string{svp::package::kSvpiVersion}},
    {"svp_version", svp_manifest.value("svp_version", "1.0-rc.2")},
    {"package_id", svp_manifest.value("package_id", "svp_extracted_pkg")},
    {"created_utc", make_utc_timestamp()},
    {"media_binding_ref", "media_binding.json"},
    {"primary_media_binding_id", binding_doc.primary_binding_id},
    {"timebase", svp_manifest.value("timebase", nlohmann::json::object())},
    {"sections", nlohmann::json::object({
      {"transcript", {{"state", "extracted"}}},
      {"timeline", {{"state", "extracted"}}},
      {"text", {{"state", "extracted"}}},
      {"colors", {{"state", "extracted"}}},
      {"entities", {{"state", "extracted"}}},
      {"spatial", {{"state", "extracted"}}},
      {"relationships", {{"state", "extracted"}}},
      {"embeddings", {{"state", "extracted"}}}
    })},
    {"extraction_source", {
      {"source_svp_path", svp_path.filename().string()},
      {"extracted_utc", make_utc_timestamp()}
    }}
  };

  std::filesystem::path staging_dir =
      out_dir / (svpi_filename + ".staging");
  std::filesystem::remove_all(staging_dir);
  std::filesystem::create_directories(staging_dir);

  const std::vector<std::string> sections_to_copy = {
    "transcript/", "timeline/", "entities/", "spatial/",
    "text/", "colors/", "relationships/", "embeddings/",
    "index/", "provenance/"
  };

  const auto num_entries = zip_get_num_entries(archive.get(), 0);
  for (zip_int64_t i = 0; i < num_entries; ++i) {
    zip_stat_t stat;
    zip_stat_init(&stat);
    if (zip_stat_index(archive.get(), static_cast<zip_uint64_t>(i), 0, &stat) != 0) {
      continue;
    }
    if (stat.name == nullptr) {
      continue;
    }
    std::string name(stat.name);

    if (name.back() == '/') {
      continue;
    }

    bool skip = true;
    for (const auto& section : sections_to_copy) {
      if (name.rfind(section, 0) == 0) {
        skip = false;
        break;
      }
    }
    if (skip) {
      continue;
    }

    auto rel_path = std::filesystem::path(name);
    auto dest = staging_dir / rel_path;
    if (!extract_zip_entry(archive.get(), name, dest)) {
      continue;
    }
  }

  {
    auto events_path = staging_dir / "provenance" / "interlace_events.jsonl";
    std::vector<nlohmann::json> existing_events;
    if (std::filesystem::exists(events_path)) {
      std::ifstream in(events_path);
      std::string line;
      while (std::getline(in, line)) {
        if (!line.empty()) {
          auto j = nlohmann::json::parse(line, nullptr, false);
          if (!j.is_discarded()) {
            existing_events.push_back(std::move(j));
          }
        }
      }
    }
    existing_events.push_back(nlohmann::json{
      {"event_id", "evt_interlace_extract_000001"},
      {"event_type", "svpi_extracted_from_svp"},
      {"event_utc", make_utc_timestamp()},
      {"authority", "package_extracted"},
      {"source_svp", svp_path.filename().string()},
      {"extracted_media", media_filename},
      {"binding_id", binding_doc.primary_binding_id},
      {"binding_contract", std::string{svp::package::kSvpiBindingContract}}
    });
    write_jsonl(events_path, existing_events);
  }

  if (!std::filesystem::exists(staging_dir / "provenance" / "processors.jsonl")) {
    write_jsonl(staging_dir / "provenance" / "processors.jsonl", {
      nlohmann::json{
        {"processor_id", "proc_interlace_extract_000001"},
        {"processor_name", "svp-builder-interlace"},
        {"processor_version", "1.0.0"},
        {"stage", "interlace-extract"},
        {"ran_utc", make_utc_timestamp()},
        {"inputs", nlohmann::json::array({svp_path.filename().string()})},
        {"outputs", nlohmann::json::array({svpi_filename, media_filename})},
        {"notes", "SVPI extracted from SVP package"}
      }
    });
  }

  auto manifest_copy = svpi_manifest;
  if (!svp::package::write_index_foundation(staging_dir, manifest_copy)) {
    result.error_message = "failed to write index foundation for extracted SVPI";
    return result;
  }

  result.success = svp::package::write_svpi_package(
      result.extracted_svpi_path, staging_dir, svpi_manifest, binding_doc);

  if (!result.success) {
    result.error_message = "failed to write extracted SVPI package";
    sink->emit(make_stage_failed(ProgressStageId::extract, result.error_message));
    return result;
  }

  sink->emit(make_artifact_written(
      ProgressStageId::extract, result.extracted_media_path, "extracted media"));
  sink->emit(make_artifact_written(
      ProgressStageId::extract, result.extracted_svpi_path, "extracted SVPI"));
  sink->emit(make_stage_completed(ProgressStageId::extract));

  return result;
}

}  // namespace svp::builder
