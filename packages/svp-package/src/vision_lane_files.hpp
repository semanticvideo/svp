#pragma once

// Small staging-file helpers shared by the vision lane stages.

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace svp::package::detail {

void write_empty_file(const std::filesystem::path& path);
void write_text_file(const std::filesystem::path& path, const std::string& content);

// Records of a JSONL file; a missing file or unparseable line contributes
// nothing.
[[nodiscard]] std::vector<nlohmann::json> read_jsonl_records(
    const std::filesystem::path& path);
void write_jsonl_records(const std::filesystem::path& path,
                         const std::vector<nlohmann::json>& records);

}  // namespace svp::package::detail
