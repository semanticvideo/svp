#pragma once

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>

namespace package_export {

// One file being written inside the staging directory. Created with
// exclusive-create and no-follow flags, so it can never overwrite or write
// through an existing file or link.
class StagedFile {
 public:
  StagedFile(int descriptor, std::string relative_path);
  StagedFile(StagedFile&& other) noexcept;
  StagedFile& operator=(StagedFile&&) = delete;
  StagedFile(const StagedFile&) = delete;
  StagedFile& operator=(const StagedFile&) = delete;
  ~StagedFile();

  void write(std::string_view bytes);
  // Flushes and closes; throws ExportError(output_write_failed) on failure.
  void close();

  [[nodiscard]] std::uint64_t size_bytes() const noexcept { return size_; }
  [[nodiscard]] const std::string& relative_path() const noexcept {
    return relative_path_;
  }

 private:
  void flush();

  int descriptor_ = -1;
  std::string relative_path_;
  std::string buffer_;
  std::uint64_t size_ = 0;
};

// Writes the export into a fresh staging directory next to --out and moves it
// onto --out only when publish() is called. Destroying an unpublished
// transaction removes the staging directory, leaving --out untouched.
class OutputTransaction {
 public:
  // Read-only checks of --out (Package_Export_v1.md Section 1). Throws
  // output_exists or output_not_replaceable.
  static void check_target(const std::filesystem::path& out, bool overwrite);

  // Refuses an --out that contains the input package, so replacing a previous
  // export can never remove the package being exported. Throws
  // output_not_replaceable.
  static void check_input_outside(const std::filesystem::path& input,
                                  const std::filesystem::path& out);

  // Throws insufficient_space when the file system holding --out reports less
  // free space than `required_bytes`.
  static void check_free_space(const std::filesystem::path& out,
                               std::uint64_t required_bytes);

  OutputTransaction(const std::filesystem::path& out, bool overwrite);
  ~OutputTransaction();

  OutputTransaction(const OutputTransaction&) = delete;
  OutputTransaction& operator=(const OutputTransaction&) = delete;
  OutputTransaction(OutputTransaction&&) = delete;
  OutputTransaction& operator=(OutputTransaction&&) = delete;

  // `relative_path` is '/'-separated and relative to the export root; missing
  // parent directories are created inside the staging directory.
  [[nodiscard]] StagedFile create_file(std::string_view relative_path);

  void publish();

 private:
  void ensure_directory(std::string_view relative_directory);

  std::filesystem::path target_;
  std::filesystem::path staging_;
  bool overwrite_ = false;
  bool published_ = false;
  std::set<std::string, std::less<>> created_directories_;
};

}  // namespace package_export
