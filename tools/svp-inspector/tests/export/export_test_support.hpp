#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace export_test {

void check(bool condition, std::string_view expression, std::string_view file,
           int line);

#define EXPORT_CHECK(condition) \
  ::export_test::check((condition), #condition, __FILE__, __LINE__)

// CTest skips a test that exits with this code (SKIP_RETURN_CODE).
inline constexpr int kSkipExitCode = 77;

class TempDir {
 public:
  explicit TempDir(std::string_view label);
  ~TempDir();
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

struct CommandResult {
  int exit_code = -1;
  std::string stdout_text;

  [[nodiscard]] nlohmann::json json() const;
};

// Runs a program (argv[0] is its path) and captures stdout.
[[nodiscard]] CommandResult run_command(const std::vector<std::string>& argv);

// `svp-inspector export <package> --out <out> [--overwrite]`.
[[nodiscard]] CommandResult run_export(const std::filesystem::path& inspector,
                                       const std::filesystem::path& package,
                                       const std::filesystem::path& out,
                                       bool overwrite = false);

[[nodiscard]] std::string read_file(const std::filesystem::path& path);
void write_file(const std::filesystem::path& path, std::string_view bytes);

// Every regular file under `root`, by '/'-separated relative path.
[[nodiscard]] std::map<std::string, std::string> read_tree(
    const std::filesystem::path& root);

// Non-blank lines of a file.
[[nodiscard]] std::vector<std::string> read_lines(
    const std::filesystem::path& path);

[[nodiscard]] std::vector<nlohmann::json> read_jsonl(
    const std::filesystem::path& path);

struct ZipEntrySpec {
  std::string name;
  std::string content;
  bool store = false;
};

void write_zip(const std::filesystem::path& path,
               const std::vector<ZipEntrySpec>& entries);

// File entry names of a ZIP (directories excluded).
[[nodiscard]] std::vector<std::string> zip_file_entries(
    const std::filesystem::path& path);

[[nodiscard]] std::string zip_entry_content(const std::filesystem::path& path,
                                            const std::string& name);

// True when hidden staging or replaced-export directories remain in `parent`.
[[nodiscard]] bool has_export_leftovers(const std::filesystem::path& parent);

// A minimal ISO BMFF file (ftyp, moov, mdat) an SVPI can be embedded into.
void write_iso_bmff_container(const std::filesystem::path& path);

}  // namespace export_test
