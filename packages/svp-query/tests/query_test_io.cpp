#include "query_test_io.hpp"

#include <filesystem>
#include <fstream>
#include <vector>

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << content;
}

void write_jsonl(const std::filesystem::path& path,
                 const std::vector<nlohmann::json>& records) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  for (const auto& r : records) {
    out << r.dump() << "\n";
  }
}

void write_json(const std::filesystem::path& path, const nlohmann::json& value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << value.dump(2) << "\n";
}
