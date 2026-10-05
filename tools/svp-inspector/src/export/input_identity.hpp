#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace package_export {

// What the file system says about the input file. Two snapshots that compare
// equal mean the file was not replaced, resized, or rewritten in between.
struct InputIdentity {
  std::uint64_t device = 0;
  std::uint64_t inode = 0;
  std::uint64_t size_bytes = 0;
  std::int64_t modified_seconds = 0;
  std::int64_t modified_nanoseconds = 0;
  std::int64_t changed_seconds = 0;
  std::int64_t changed_nanoseconds = 0;
  bool is_regular_file = false;

  friend bool operator==(const InputIdentity&, const InputIdentity&) = default;
};

// Follows symlinks, like the validators do. Returns nullopt when the path
// does not exist or cannot be examined.
[[nodiscard]] std::optional<InputIdentity> read_input_identity(
    const std::filesystem::path& path);

}  // namespace package_export
