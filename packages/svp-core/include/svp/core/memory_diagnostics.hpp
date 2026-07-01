#pragma once

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace svp::core {

struct MemorySnapshot {
  std::uint64_t resident_bytes = 0;
  std::uint64_t virtual_bytes = 0;
  std::uint64_t physical_footprint_bytes = 0;
};

MemorySnapshot current_memory_snapshot();

void configure_memory_diagnostics_from_environment(
    const std::filesystem::path& default_log_path = {});

void configure_memory_diagnostics(
    const std::filesystem::path& log_path,
    std::uint64_t limit_bytes);

bool memory_diagnostics_enabled();
std::filesystem::path memory_diagnostics_log_path();
std::uint64_t memory_diagnostics_limit_bytes();

void trace_memory_event(
    std::string_view scope,
    std::initializer_list<std::pair<std::string, std::string>> fields = {});

void trace_memory_event(
    std::string_view scope,
    const std::vector<std::pair<std::string, std::string>>& fields);

void check_memory_limit(
    std::string_view scope,
    std::initializer_list<std::pair<std::string, std::string>> fields = {});

void check_memory_limit(
    std::string_view scope,
    const std::vector<std::pair<std::string, std::string>>& fields);

}  // namespace svp::core
