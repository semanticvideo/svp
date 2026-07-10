#pragma once

#include "svp/package/embedded_svpi.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace svp::package::detail {

struct TopLevelBox {
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
  std::uint64_t header_size = 0;
  std::array<char, 4> type{};
  bool extends_to_eof = false;
};

struct TopLevelScan {
  std::uint64_t file_size = 0;
  std::uint64_t bytes_read = 0;
  bool readable = false;
  bool valid = false;
  std::vector<TopLevelBox> boxes;
  std::vector<EmbeddedSvpiIssue> issues;
};

[[nodiscard]] TopLevelScan scan_top_level_boxes(const std::filesystem::path& path);
[[nodiscard]] bool read_exact_at(std::ifstream& input, std::uint64_t offset,
                                 void* destination, std::size_t size);
[[nodiscard]] std::uint32_t read_be32(const std::uint8_t* bytes) noexcept;
[[nodiscard]] std::uint64_t read_be64(const std::uint8_t* bytes) noexcept;
void write_be16(std::uint8_t* bytes, std::uint16_t value) noexcept;
void write_be32(std::uint8_t* bytes, std::uint32_t value) noexcept;
void write_be64(std::uint8_t* bytes, std::uint64_t value) noexcept;

}  // namespace svp::package::detail
