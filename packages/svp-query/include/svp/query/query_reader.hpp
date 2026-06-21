#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace svp::query {

struct JsonlReadResult {
  bool present = false;
  bool readable = false;
  bool has_malformed = false;
  std::uint64_t malformed_line_count = 0;
  std::string error_message;
  std::vector<nlohmann::json> records;
};

struct JsonReadResult {
  bool present = false;
  bool readable = false;
  bool parsed = false;
  std::string error_message;
  nlohmann::json value;
};

[[nodiscard]] JsonlReadResult read_jsonl_entry(const std::filesystem::path& package_path,
                                                const std::string& entry);

[[nodiscard]] JsonReadResult read_json_entry(const std::filesystem::path& package_path,
                                              const std::string& entry);

}  // namespace svp::query
