#include "embedded_svpi_test_support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <string>

namespace svp::package::test {
namespace {

void put_be32(std::ostream& output, std::uint32_t value) {
  const std::array<char, 4> bytes{
      static_cast<char>(value >> 24U), static_cast<char>(value >> 16U),
      static_cast<char>(value >> 8U), static_cast<char>(value)};
  output.write(bytes.data(), bytes.size());
}

void put_be64(std::ostream& output, std::uint64_t value) {
  put_be32(output, static_cast<std::uint32_t>(value >> 32U));
  put_be32(output, static_cast<std::uint32_t>(value));
}

void put_box(std::ostream& output, std::string_view type,
             const std::vector<std::uint8_t>& payload = {}) {
  put_be32(output, static_cast<std::uint32_t>(8 + payload.size()));
  output.write(type.data(), 4);
  output.write(reinterpret_cast<const char*>(payload.data()),
               static_cast<std::streamsize>(payload.size()));
}

void put_unrelated_uuid(std::ostream& output) {
  put_be32(output, 24);
  output.write("uuid", 4);
  const std::array<char, 16> uuid{
      1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  output.write(uuid.data(), uuid.size());
}

}  // namespace

TempDirectory::TempDirectory() {
  const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  path = std::filesystem::temp_directory_path() /
         ("svp-embedded-transport-tests-" + std::to_string(suffix));
  std::filesystem::create_directories(path);
}

TempDirectory::~TempDirectory() {
  std::error_code ignored;
  std::filesystem::remove_all(path, ignored);
}

void check(bool condition, std::string_view expression,
           std::string_view file, int line) {
  if (!condition) {
    throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                             ": check failed: " + std::string(expression));
  }
}

void write_test_iso_bmff(const std::filesystem::path& path,
                    bool moov_before_mdat,
                    bool terminal_mfra,
                    bool zero_sized_mdat,
                    bool unrelated_uuid,
                    std::string_view major_brand) {
  std::ofstream output(path, std::ios::binary);
  std::vector<std::uint8_t> ftyp(12);
  std::copy_n(major_brand.begin(), 4, ftyp.begin());
  ftyp[7] = 1;
  std::copy_n(major_brand.begin(), 4, ftyp.begin() + 8);
  put_box(output, "ftyp", ftyp);
  if (moov_before_mdat) {
    put_box(output, "moov");
  }
  if (zero_sized_mdat) {
    put_be32(output, 0);
    output.write("mdat", 4);
    output.write("media", 5);
  } else {
    put_box(output, "mdat", {'m', 'e', 'd', 'i', 'a'});
  }
  if (!moov_before_mdat && !zero_sized_mdat) {
    put_box(output, "moov");
  }
  if (unrelated_uuid && !zero_sized_mdat) {
    put_unrelated_uuid(output);
  }
  if (terminal_mfra && !zero_sized_mdat) {
    put_box(output, "mfra", {'t', 'a', 'i', 'l'});
  }
}

void write_sparse_iso_bmff(const std::filesystem::path& path,
                           std::uint64_t mdat_payload_size) {
  std::ofstream output(path, std::ios::binary);
  put_box(output, "ftyp", {'i', 's', 'o', 'm', 0, 0, 0, 0,
                           'i', 's', 'o', 'm'});
  put_be32(output, 1);
  output.write("mdat", 4);
  put_be64(output, 16 + mdat_payload_size);
  output.seekp(static_cast<std::streamoff>(mdat_payload_size - 1), std::ios::cur);
  output.put('\0');
  put_box(output, "moov");
}

void write_bytes(const std::filesystem::path& path,
                 const std::vector<std::uint8_t>& bytes) {
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  const auto size = input.tellg();
  input.seekg(0);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  input.read(reinterpret_cast<char*>(bytes.data()), size);
  return bytes;
}

void overwrite_byte(const std::filesystem::path& path,
                    std::uint64_t offset, std::uint8_t value) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  file.seekp(static_cast<std::streamoff>(offset));
  file.put(static_cast<char>(value));
}

void overwrite_be64(const std::filesystem::path& path,
                    std::uint64_t offset, std::uint64_t value) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  file.seekp(static_cast<std::streamoff>(offset));
  put_be64(file, value);
}

void overwrite_be32(const std::filesystem::path& path,
                    std::uint64_t offset, std::uint32_t value) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  file.seekp(static_cast<std::streamoff>(offset));
  put_be32(file, value);
}

bool has_issue(const EmbeddedSvpiInspection& inspection,
               EmbeddedSvpiIssueCode code) {
  return std::ranges::any_of(inspection.issues, [&](const auto& issue) {
    return issue.code == code;
  });
}

}  // namespace svp::package::test
