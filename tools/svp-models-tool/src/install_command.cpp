#include "install_command.hpp"

#include "install_process.hpp"
#include "install_resources.hpp"

#include "svp/progress/event.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace svp::models::tool {
namespace {

constexpr std::string_view kPythonUrl =
    "https://github.com/astral-sh/python-build-standalone/releases/download/"
    "20250612/cpython-3.11.13%2B20250612-aarch64-apple-darwin-install_only.tar.gz";
constexpr std::string_view kPythonSha256 =
    "e272f0baca8f5a3cef29cc9c7418b80d0316553062ad3235205a33992155043c";
constexpr std::uintmax_t kPythonBytes = 18067291;

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    std::array<char, 42> pattern{};
    const std::string value = "/tmp/svp-model-bootstrap.XXXXXX";
    std::copy(value.begin(), value.end(), pattern.begin());
    const char* created = mkdtemp(pattern.data());
    if (!created) throw std::runtime_error("could not create installer temporary directory");
    path_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

std::string trimmed(std::string value) {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r' ||
                            value.back() == ' ' || value.back() == '\t')) {
    value.pop_back();
  }
  return value;
}

void require_platform() {
  const auto system = run_process({"/usr/bin/uname", "-s"});
  const auto machine = run_process({"/usr/bin/uname", "-m"});
  if (system.exit_code != 0 || trimmed(system.output) != "Darwin" ||
      machine.exit_code != 0 || trimmed(machine.output) != "arm64") {
    throw std::runtime_error(
        "reference-model installation currently supports Apple Silicon macOS only");
  }
}

svp::progress::EventKind parse_kind(const std::string& value) {
  if (value == "stage_started") return svp::progress::EventKind::started;
  if (value == "stage_completed") return svp::progress::EventKind::completed;
  if (value == "stage_failed") return svp::progress::EventKind::failed;
  if (value == "stage_progress") return svp::progress::EventKind::progress;
  if (value == "warning") return svp::progress::EventKind::warning;
  if (value == "artifact_written") return svp::progress::EventKind::artifact_written;
  throw std::runtime_error("installer emitted an unknown progress event kind");
}

svp::progress::Event parse_event(const std::string& line) {
  const auto value = nlohmann::json::parse(line);
  svp::progress::Event event;
  event.kind = parse_kind(value.at("kind").get<std::string>());
  event.stage_id = value.at("stage").get<std::string>();
  event.stage_label = value.at("stage_label").get<std::string>();
  if (value.contains("message")) event.message = value.at("message").get<std::string>();
  if (value.contains("current")) event.current = value.at("current").get<std::uint64_t>();
  if (value.contains("total")) event.total = value.at("total").get<std::uint64_t>();
  if (value.contains("fraction")) event.fraction = value.at("fraction").get<double>();
  if (value.contains("unit")) event.unit = value.at("unit").get<std::string>();
  if (value.contains("scope_id")) event.scope_id = value.at("scope_id").get<std::string>();
  if (value.contains("scope_label")) event.scope_label = value.at("scope_label").get<std::string>();
  return event;
}

void require_success(const ProcessResult& result, std::string_view action) {
  if (result.exit_code == 0) return;
  if (result.exit_code == 130) throw std::runtime_error("installation cancelled");
  throw std::runtime_error(std::string(action) + " failed: " + trimmed(result.output));
}

}  // namespace

int install_reference_models(const std::filesystem::path& executable,
                             const std::filesystem::path& cache_dir,
                             int parallel_downloads,
                             svp::progress::Mode progress_mode,
                             std::ostream& progress_stream,
                             bool is_tty,
                             int terminal_fd) {
  require_platform();
  if (parallel_downloads < 1 || parallel_downloads > 2) {
    throw std::invalid_argument("--parallel-downloads must be 1 or 2");
  }

  auto sink = svp::progress::make_sink(
      progress_mode, progress_stream, is_tty, terminal_fd);
  TemporaryDirectory temporary;
  const auto archive = temporary.path() / "python.tar.gz";
  sink->emit({.kind = svp::progress::EventKind::started,
              .stage_id = "bootstrap_download",
              .stage_label = "Installer Runtime"});
  const auto download = run_process({
      "/usr/bin/curl", "--silent", "--show-error", "--location", "--fail",
      "--output", archive.string(), std::string(kPythonUrl)});
  require_success(download, "installer runtime download");
  if (std::filesystem::file_size(archive) != kPythonBytes) {
    throw std::runtime_error("installer runtime byte count did not match its lock");
  }
  const auto hash = run_process({"/usr/bin/shasum", "-a", "256", archive.string()});
  require_success(hash, "installer runtime verification");
  if (!trimmed(hash.output).starts_with(kPythonSha256)) {
    throw std::runtime_error("installer runtime SHA-256 did not match its lock");
  }
  sink->emit({.kind = svp::progress::EventKind::completed,
              .stage_id = "bootstrap_download",
              .stage_label = "Installer Runtime"});

  const auto runtime_root = temporary.path() / "runtime";
  std::filesystem::create_directories(runtime_root);
  const auto extract = run_process({
      "/usr/bin/tar", "-xzf", archive.string(), "-C", runtime_root.string()});
  require_success(extract, "installer runtime extraction");
  const auto resources = write_install_resources(temporary.path() / "resources");
  const auto python = runtime_root / "python" / "bin" / "python3";

  std::error_code canonical_error;
  const auto canonical_executable = std::filesystem::canonical(executable, canonical_error);
  if (canonical_error) throw std::runtime_error("could not resolve svp-models-tool path");
  const auto result = run_process({
      python.string(), resources.workflow_script.string(),
      "--catalog", resources.catalog.string(),
      "--bundle-inputs", resources.bundle_inputs.string(),
      "--reference-set", resources.reference_set.string(),
      "--prepare-script", resources.prepare_script.string(),
      "--ppocr-lock", resources.ppocr_lock.string(),
      "--rfdetr-lock", resources.rfdetr_lock.string(),
      "--models-tool", canonical_executable.string(),
      "--cache-dir", cache_dir.string(),
      "--parallel-downloads", std::to_string(parallel_downloads),
  }, [&](const std::string& line) {
    try {
      sink->emit(parse_event(line));
    } catch (const nlohmann::json::exception&) {
      throw std::runtime_error("installer emitted invalid progress data: " + line);
    }
  });
  if (result.exit_code == 130) return 130;
  require_success(result, "reference-model installation");
  return 0;
}

}  // namespace svp::models::tool
