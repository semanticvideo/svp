#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

struct zip;

namespace svp::validation::equivalence {

// Read-only libzip handle kept open for the duration of one comparison, so
// large entries can be streamed instead of loaded whole.
class PackageArchive {
 public:
  explicit PackageArchive(const std::filesystem::path& path);
  ~PackageArchive();

  PackageArchive(const PackageArchive&) = delete;
  PackageArchive& operator=(const PackageArchive&) = delete;

  [[nodiscard]] std::string read_entry(const std::string& entry) const;

  // Streams both entries and returns the first differing byte offset, or
  // nullopt when the entries are byte-identical.
  [[nodiscard]] static std::optional<std::uint64_t> first_difference(
      const PackageArchive& left,
      const PackageArchive& right,
      const std::string& entry);

 private:
  struct zip* archive_ = nullptr;
};

struct FileIdentity {
  std::uint64_t size_bytes = 0;
  std::string blake3;  // "blake3:<64 lowercase hex>"
};

[[nodiscard]] FileIdentity file_identity(const std::filesystem::path& path);

}  // namespace svp::validation::equivalence
