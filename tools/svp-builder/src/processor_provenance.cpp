#include "processor_provenance.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>

namespace svp::builder {
namespace fs = std::filesystem;

namespace {

constexpr std::string_view kProcessorsJsonl = "provenance/processors.jsonl";
constexpr std::string_view kFragmentExtension = ".jsonl";

std::vector<nlohmann::json> read_jsonl(const fs::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot read processor records: " + path.string());
  }
  std::vector<nlohmann::json> records;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty()) {
      records.push_back(nlohmann::json::parse(line));
    }
  }
  return records;
}

void write_jsonl(const fs::path& path, const std::vector<nlohmann::json>& records) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("cannot write processor records: " + path.string());
  }
  for (const nlohmann::json& record : records) {
    output << record.dump() << "\n";
  }
  if (!output) {
    throw std::runtime_error("cannot write processor records: " + path.string());
  }
}

// Every fragment's records, fragments in file name order.
std::vector<std::vector<nlohmann::json>> read_fragments(const fs::path& fragment_dir) {
  std::vector<fs::path> paths;
  std::error_code error;
  if (fs::is_directory(fragment_dir, error)) {
    for (const auto& entry : fs::directory_iterator(fragment_dir)) {
      if (entry.is_regular_file() && entry.path().extension() == kFragmentExtension) {
        paths.push_back(entry.path());
      }
    }
  }
  std::sort(paths.begin(), paths.end());
  std::vector<std::vector<nlohmann::json>> fragments;
  fragments.reserve(paths.size());
  for (const fs::path& path : paths) {
    fragments.push_back(read_jsonl(path));
  }
  return fragments;
}

}  // namespace

std::string stage_processor_fragment_ref(std::string_view stage) {
  return std::string(kStageProcessorFragmentDir) + std::string(stage) +
         std::string(kFragmentExtension);
}

fs::path stage_processor_fragment_path(const fs::path& staging_dir, std::string_view stage) {
  return staging_dir / stage_processor_fragment_ref(stage);
}

void write_stage_processor_fragment(const fs::path& staging_dir, std::string_view stage,
                                    const std::vector<nlohmann::json>& records) {
  write_jsonl(stage_processor_fragment_path(staging_dir, stage), records);
}

ProcessorRecordMerge merge_processor_records_canonically(
    const std::vector<std::vector<nlohmann::json>>& contributions) {
  ProcessorRecordMerge merge;
  // id -> (compact serialization, record); std::map keeps ids sorted.
  std::map<std::string, std::pair<std::string, const nlohmann::json*>> by_id;
  for (const std::vector<nlohmann::json>& contribution : contributions) {
    for (const nlohmann::json& record : contribution) {
      const auto id = record.find("id");
      if (id == record.end() || !id->is_string()) {
        continue;
      }
      std::string serialized = record.dump();
      const auto [slot, inserted] =
          by_id.try_emplace(id->get<std::string>(), serialized, &record);
      if (inserted) {
        continue;
      }
      ++merge.duplicates_merged;
      if (serialized < slot->second.first) {
        slot->second = {std::move(serialized), &record};
      }
    }
  }
  merge.records.reserve(by_id.size());
  for (const auto& [id, kept] : by_id) {
    merge.records.push_back(*kept.second);
  }
  return merge;
}

ProcessorRecordMerge compose_processor_provenance(
    const fs::path& staging_dir, const std::vector<nlohmann::json>& lane_records) {
  const fs::path fragment_dir = staging_dir / kStageProcessorFragmentDir;
  std::vector<std::vector<nlohmann::json>> contributions = read_fragments(fragment_dir);
  contributions.push_back(lane_records);
  ProcessorRecordMerge merge = merge_processor_records_canonically(contributions);
  write_jsonl(staging_dir / kProcessorsJsonl, merge.records);
  fs::remove_all(fragment_dir);
  return merge;
}

}  // namespace svp::builder
