#pragma once

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace svp::package {

struct PackageLayout {
  std::set<std::string> entries;
  std::set<std::string> root_entries;
  std::vector<std::string> invalid_entry_paths;

  [[nodiscard]] bool has_entry(const std::string& entry) const;
  [[nodiscard]] bool has_top_level_section(const std::string& section) const;
};

class PackageLayoutResult {
 public:
  [[nodiscard]] static PackageLayoutResult success(PackageLayout layout);
  [[nodiscard]] static PackageLayoutResult failure(std::string message);

  [[nodiscard]] bool has_value() const noexcept;
  [[nodiscard]] const PackageLayout& value() const;
  [[nodiscard]] const std::string& error_message() const noexcept;

 private:
  PackageLayout layout_;
  std::string error_message_;
  bool has_value_ = false;
};

class PackageEntryReadResult {
 public:
  [[nodiscard]] static PackageEntryReadResult success(std::string content);
  [[nodiscard]] static PackageEntryReadResult failure(std::string message);

  [[nodiscard]] bool has_value() const noexcept;
  [[nodiscard]] const std::string& value() const;
  [[nodiscard]] const std::string& error_message() const noexcept;

 private:
  std::string content_;
  std::string error_message_;
  bool has_value_ = false;
};

[[nodiscard]] PackageLayoutResult read_package_layout(const std::filesystem::path& path);
[[nodiscard]] PackageEntryReadResult read_package_entry(const std::filesystem::path& path,
                                                        const std::string& entry);

}  // namespace svp::package
