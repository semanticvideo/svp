#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace svp::validation {

struct JsonLineRecord {
  nlohmann::json value;
  std::size_t line = 0;
};

struct JsonLinesReadResult {
  std::vector<JsonLineRecord> records;
  std::string error_message;

  [[nodiscard]] bool has_value() const noexcept {
    return error_message.empty();
  }
};

[[nodiscard]] JsonLinesReadResult read_json_lines_from_package(
    const std::filesystem::path& package_path,
    const std::string& entry);

}  // namespace svp::validation
