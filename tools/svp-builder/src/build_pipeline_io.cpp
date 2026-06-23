#include "build_pipeline_internal.hpp"

#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>

namespace svp::builder {

void write_json_file(const std::filesystem::path& output_path,
                     const nlohmann::json& value) {
  const std::filesystem::path parent = output_path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }

  std::ofstream output(output_path);
  if (!output) {
    throw std::runtime_error("unable to open output path: " + output_path.string());
  }
  output << value.dump(2) << "\n";
}

void write_jsonl_file(const std::filesystem::path& output_path,
                      const nlohmann::json& records) {
  if (!records.is_array()) {
    throw std::runtime_error("jsonl staging records must be an array");
  }

  const std::filesystem::path parent = output_path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }

  std::ofstream output(output_path);
  if (!output) {
    throw std::runtime_error("unable to open output path: " + output_path.string());
  }

  for (const nlohmann::json& record : records) {
    output << record.dump() << "\n";
  }
}

void append_jsonl_file(const std::filesystem::path& output_path,
                       const nlohmann::json& records) {
  if (!records.is_array()) {
    throw std::runtime_error("jsonl staging records must be an array");
  }

  const std::filesystem::path parent = output_path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }

  std::ofstream output(output_path, std::ios::app);
  if (!output) {
    throw std::runtime_error("unable to open output path: " + output_path.string());
  }

  for (const nlohmann::json& record : records) {
    output << record.dump() << "\n";
  }
}

svp::media::MediaProbe load_or_run_probe(const std::string& source_path,
                                         const std::string& probe_json_path,
                                         const std::string& ffprobe_path) {
  if (!probe_json_path.empty()) {
    return svp::media::load_media_probe_json(probe_json_path);
  }
  return svp::media::probe_media_with_ffprobe(source_path, ffprobe_path);
}

bool executable_exists(const std::filesystem::path& executable_path) {
  if (executable_path.empty()) {
    return false;
  }
  if (executable_path.has_parent_path()) {
    return std::filesystem::exists(executable_path);
  }

  const char* path_env = std::getenv("PATH");
  if (path_env == nullptr) {
    return false;
  }

  std::string paths(path_env);
  std::size_t start = 0;
  while (start <= paths.size()) {
    const std::size_t end = paths.find(':', start);
    const std::string entry =
        paths.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!entry.empty() && std::filesystem::exists(std::filesystem::path(entry) / executable_path)) {
      return true;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }

  return false;
}



}  // namespace svp::builder
