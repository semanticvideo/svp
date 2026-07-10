#pragma once

#include "svp/package/embedded_svpi.hpp"

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace svp::package::test {

struct TempDirectory {
  std::filesystem::path path;
  TempDirectory();
  ~TempDirectory();
};

void check(bool condition, std::string_view expression,
           std::string_view file, int line);

void write_test_iso_bmff(const std::filesystem::path& path,
                    bool moov_before_mdat,
                    bool terminal_mfra = false,
                    bool zero_sized_mdat = false,
                    bool unrelated_uuid = false,
                    std::string_view major_brand = "isom");
void write_sparse_iso_bmff(const std::filesystem::path& path,
                           std::uint64_t mdat_payload_size);
void write_bytes(const std::filesystem::path& path,
                 const std::vector<std::uint8_t>& bytes);
std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path);
void overwrite_byte(const std::filesystem::path& path,
                    std::uint64_t offset, std::uint8_t value);
void overwrite_be64(const std::filesystem::path& path,
                    std::uint64_t offset, std::uint64_t value);
void overwrite_be32(const std::filesystem::path& path,
                    std::uint64_t offset, std::uint32_t value);
bool has_issue(const EmbeddedSvpiInspection& inspection,
               EmbeddedSvpiIssueCode code);

}  // namespace svp::package::test

#define CHECK_EMBEDDED(expr) \
  ::svp::package::test::check((expr), #expr, __FILE__, __LINE__)
