#include "install_command.hpp"

#include "executable_path.hpp"
#include "install_process.hpp"
#include "install_resources.hpp"

#include "svp/models/model_lock.hpp"
#include "svp/models/reference_model_set.hpp"
#include "svp/models/verification.hpp"
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
  throw std::runtime_error(std::string(action) + " failed: " + trimmed(result.output));
}

std::string verification_errors(const svp::models::VerificationReport& report) {
  std::string result;
  for (const auto& issue : report.issues) {
    if (issue.severity != svp::models::VerificationSeverity::error) continue;
    if (!result.empty()) result += "; ";
    result += issue.message;
  }
  return result;
}

}  // namespace

int install_reference_models(const std::filesystem::path& executable,
                             const std::filesystem::path& cache_dir,
                             int parallel_downloads,
                             svp::progress::Mode progress_mode,
                             std::ostream& progress_stream,
                             bool is_tty,
                             int terminal_fd) {
  if (parallel_downloads < 1 || parallel_downloads > 2) {
    throw std::invalid_argument("--parallel-downloads must be 1 or 2");
  }

  auto sink = svp::progress::make_sink(
      progress_mode, progress_stream, is_tty, terminal_fd);
  const auto canonical_executable = resolve_current_executable(executable);
  TemporaryDirectory temporary;
  const auto resources = write_install_resources(temporary.path() / "resources");
  if (std::filesystem::exists(cache_dir)) {
    const auto lock_path = cache_dir / "model-lock.json";
    if (!std::filesystem::is_regular_file(lock_path)) {
      throw std::runtime_error("existing model cache is missing authoritative "
                               "model-lock.json: " + lock_path.string());
    }
    const auto reference_set =
        svp::models::load_reference_model_set(resources.reference_set);
    const auto report = svp::models::verify_locked_reference_set_against_cache(
        svp::models::load_model_lock(lock_path),
        reference_set,
        cache_dir);
    if (!report.ok()) {
      throw std::runtime_error("existing model cache is not the exact reference set: " +
                               verification_errors(report));
    }
    sink->emit({.kind = svp::progress::EventKind::completed,
                .stage_id = "models",
                .stage_label = "Models",
                .message = "already verified at " + cache_dir.string(),
                .current = reference_set.models.size(),
                .total = reference_set.models.size(),
                .unit = "models"});
    return 0;
  }

  require_platform();
  const auto archive = temporary.path() / "python.tar.gz";
  sink->emit({.kind = svp::progress::EventKind::started,
              .stage_id = "bootstrap_download",
              .stage_label = "Installer Runtime"});
  const auto download = run_process({
      "/usr/bin/curl", "--silent", "--show-error", "--location", "--fail",
      "--output", archive.string(), std::string(kPythonUrl)});
  if (download.exit_code == 130) return 130;
  require_success(download, "installer runtime download");
  if (std::filesystem::file_size(archive) != kPythonBytes) {
    throw std::runtime_error("installer runtime byte count did not match its lock");
  }
  const auto hash = run_process({"/usr/bin/shasum", "-a", "256", archive.string()});
  if (hash.exit_code == 130) return 130;
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
  if (extract.exit_code == 130) return 130;
  require_success(extract, "installer runtime extraction");
  const auto python = runtime_root / "python" / "bin" / "python3";
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
