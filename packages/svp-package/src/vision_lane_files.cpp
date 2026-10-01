#include "vision_lane_files.hpp"

#include <fstream>

namespace svp::package::detail {

void write_empty_file(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
}

void write_text_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output << content;
}

std::vector<nlohmann::json> read_jsonl_records(const std::filesystem::path& path) {
  std::vector<nlohmann::json> records;
  if (!std::filesystem::exists(path)) {
    return records;
  }
  std::ifstream input(path);
  if (!input) {
    return records;
  }
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) {
      continue;
    }
    try {
      records.push_back(nlohmann::json::parse(line));
    } catch (...) {
    }
  }
  return records;
}

void write_jsonl_records(const std::filesystem::path& path,
                         const std::vector<nlohmann::json>& records) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  for (const nlohmann::json& record : records) {
    output << record.dump() << "\n";
  }
}

}  // namespace svp::package::detail
