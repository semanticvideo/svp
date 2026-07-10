#pragma once

#include "svp/package/embedded_svpi.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace svp::package::detail {

struct TemporaryOutput {
  std::filesystem::path path;
  bool committed = false;
  ~TemporaryOutput();
};

[[nodiscard]] bool make_temporary_output(
    const std::filesystem::path& output_path,
    TemporaryOutput& temporary,
    std::string& error_message);
[[nodiscard]] bool finalize_temporary_output(
    TemporaryOutput& temporary,
    const std::filesystem::path& output_path,
    std::string& error_message);
[[nodiscard]] bool copy_bytes(std::ifstream& input, std::ofstream& output,
                              std::uint64_t offset, std::uint64_t size);
[[nodiscard]] bool hash_file(const std::filesystem::path& path,
                             std::uint64_t& size,
                             std::array<std::uint8_t, 32>& hash);
[[nodiscard]] bool output_is_allowed(const std::filesystem::path& output_path,
                                     bool overwrite,
                                     std::string& error_message);
[[nodiscard]] EmbeddedSvpiOperationResult failure_result(
    const std::filesystem::path& output_path,
    EmbeddedSvpiInspection inspection,
    std::string message);

}  // namespace svp::package::detail
