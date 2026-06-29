#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

void write_file(const std::filesystem::path& path, const std::string& content);

void write_jsonl(const std::filesystem::path& path,
                 const std::vector<nlohmann::json>& records);

void write_json(const std::filesystem::path& path, const nlohmann::json& value);
