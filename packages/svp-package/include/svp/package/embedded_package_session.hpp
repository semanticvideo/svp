#pragma once

#include "svp/package/embedded_svpi.hpp"

#include <cstdint>
#include <filesystem>

namespace svp::package {

class VerifiedEmbeddedPackageSession {
 public:
  explicit VerifiedEmbeddedPackageSession(
      const std::filesystem::path& path);
  ~VerifiedEmbeddedPackageSession();

  VerifiedEmbeddedPackageSession(
      const VerifiedEmbeddedPackageSession&) = delete;
  VerifiedEmbeddedPackageSession& operator=(
      const VerifiedEmbeddedPackageSession&) = delete;
  VerifiedEmbeddedPackageSession(
      VerifiedEmbeddedPackageSession&&) = delete;
  VerifiedEmbeddedPackageSession& operator=(
      VerifiedEmbeddedPackageSession&&) = delete;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] const EmbeddedSvpiInspection& inspection() const noexcept;
  [[nodiscard]] bool matches(const std::filesystem::path& path) const;
  [[nodiscard]] std::uint64_t payload_offset() const noexcept;
  [[nodiscard]] std::uint64_t payload_size() const noexcept;
  [[nodiscard]] static bool active_range_for(
      const std::filesystem::path& path,
      std::uint64_t& payload_offset,
      std::uint64_t& payload_size);

 private:
  std::filesystem::path path_;
  EmbeddedSvpiInspection inspection_;
  VerifiedEmbeddedPackageSession* previous_ = nullptr;
  std::uint64_t device_ = 0;
  std::uint64_t inode_ = 0;
  std::uint64_t file_size_ = 0;
  std::int64_t modified_seconds_ = 0;
  std::int64_t modified_nanoseconds_ = 0;
  std::int64_t changed_seconds_ = 0;
  std::int64_t changed_nanoseconds_ = 0;
  bool active_ = false;
  static thread_local VerifiedEmbeddedPackageSession* current_;
};

}  // namespace svp::package
