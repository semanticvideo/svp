// Output path policy for builder artifacts.
//
// 1. Every spelling of an output path must behave the same: a bare filename
//    ("clip.svp") is "./clip.svp", and nested relative or absolute paths get
//    their missing parent directories created.
// 2. A package or artifact that cannot be written must fail the command with a
//    non-zero exit status and must never print a success line.
//
// argv[1] is the svp-builder executable, used for the command-line checks.

#include "svp/builder/build_pipeline.hpp"
#include "svp/builder/interlace.hpp"
#include "svp/package/output_directory.hpp"

#include "model_cache_test_fixture.hpp"
#include "pipeline_input_fixture.hpp"

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

namespace fs = std::filesystem;

// Directory permissions that allow listing and traversal but not creating
// entries, used to force artifact writes to fail.
constexpr fs::perms kReadOnlyDirectory =
    fs::perms::owner_read | fs::perms::owner_exec;

class ScopedCwd {
 public:
  explicit ScopedCwd(const fs::path& path) : saved_(fs::current_path()) {
    fs::create_directories(path);
    fs::current_path(path);
  }
  ~ScopedCwd() { fs::current_path(saved_); }

 private:
  fs::path saved_;
};

class ReadOnlyDirectory {
 public:
  explicit ReadOnlyDirectory(const fs::path& path) : path_(path) {
    fs::create_directories(path_);
    fs::permissions(path_, kReadOnlyDirectory, fs::perm_options::replace);
  }
  ~ReadOnlyDirectory() {
    fs::permissions(path_, fs::perms::owner_all, fs::perm_options::replace);
  }
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

// One way a user can spell an output path, relative to the process cwd.
struct OutputSpelling {
  std::string label;
  std::function<fs::path(const fs::path& cwd, const std::string& name)> make;
};

std::vector<OutputSpelling> output_spellings() {
  return {
      {"bare", [](const fs::path&, const std::string& name) {
         return fs::path(name);
       }},
      {"dot", [](const fs::path&, const std::string& name) {
         return fs::path(".") / name;
       }},
      {"nested", [](const fs::path&, const std::string& name) {
         return fs::path("nested") / "deeper" / name;
       }},
      {"absolute", [](const fs::path& cwd, const std::string& name) {
         return cwd / "absolute-parent" / name;
       }},
  };
}

fs::path make_root() {
  const fs::path root = fs::temp_directory_path() /
                        ("svp-output-path-tests-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root);
  return root;
}

std::string read_file(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

struct PipelineInputs {
  fs::path media;
  fs::path probe;
  fs::path model_cache;
};

PipelineInputs write_pipeline_inputs(const fs::path& root) {
  const fs::path inputs = root / "inputs";
  fs::create_directories(inputs);
  return {
      .media = svp::builder::test::write_mock_media_file(inputs),
      .probe = svp::builder::test::write_minimal_probe_json(inputs),
      .model_cache =
          svp::builder::test::write_valid_model_cache(inputs / "model-cache"),
  };
}

svp::builder::BuildPipelineResult run_package_build(
    const PipelineInputs& inputs, const fs::path& output_path) {
  svp::builder::BuildPipelineOptions options;
  options.source_path = inputs.media.string();
  options.probe_json_path = inputs.probe.string();
  options.ffmpeg_path = "/usr/bin/true";
  options.output_path = output_path;
  options.model_cache_dir = inputs.model_cache;
  options.stop_after = svp::builder::BuildStage::package_skeleton;
  options.force_single_speaker = true;
  return svp::builder::BuildPipeline{}.run(options);
}

svp::builder::InterlaceCreateResult run_core_only_svpi(
    const PipelineInputs& inputs, const fs::path& output_path) {
  svp::builder::InterlaceCreateOptions options;
  options.source_path = inputs.media.string();
  options.output_path = output_path.string();
  options.ffprobe_path = "/usr/bin/true";
  options.compute_chunk_proof = false;
  options.core_only_diagnostic = true;
  return svp::builder::interlace_create(options);
}

struct CommandResult {
  int exit_code = -1;
  std::string out;
  std::string err;
};

// Runs the builder in the current working directory, capturing its output.
CommandResult run_builder(const fs::path& builder,
                          const std::vector<std::string>& args,
                          const fs::path& capture_dir) {
  fs::create_directories(capture_dir);
  const fs::path out_path = capture_dir / "stdout.txt";
  const fs::path err_path = capture_dir / "stderr.txt";

  std::vector<std::string> storage{builder.string()};
  storage.insert(storage.end(), args.begin(), args.end());
  std::vector<char*> argv;
  for (auto& arg : storage) argv.push_back(arg.data());
  argv.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
                                   out_path.c_str(),
                                   O_WRONLY | O_CREAT | O_TRUNC, 0644);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO,
                                   err_path.c_str(),
                                   O_WRONLY | O_CREAT | O_TRUNC, 0644);
  pid_t pid = 0;
  const int spawn_error = posix_spawn(&pid, builder.c_str(), &actions, nullptr,
                                      argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  assert(spawn_error == 0);

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    assert(errno == EINTR);
  }
  CommandResult result;
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  result.out = read_file(out_path);
  result.err = read_file(err_path);
  return result;
}

bool printed_success(const CommandResult& result) {
  return result.out.find(" created") != std::string::npos;
}

void fail(const std::string& message, const CommandResult& result) {
  std::cerr << message << "\nexit=" << result.exit_code
            << "\nstdout:\n" << result.out << "\nstderr:\n" << result.err
            << "\n";
  std::abort();
}

// --- ensure_parent_directory -------------------------------------------------

void test_parent_directory_policy_accepts_every_spelling(const fs::path& root) {
  const fs::path cwd = root / "policy";
  ScopedCwd scoped(cwd);
  for (const auto& spelling : output_spellings()) {
    const fs::path output = spelling.make(cwd, "artifact.bin");
    svp::package::ensure_parent_directory(output);
    const fs::path parent = output.parent_path();
    assert(parent.empty() || fs::is_directory(parent));
    std::ofstream(output) << "x";
    assert(fs::exists(output));
  }
  std::cout << "  test_parent_directory_policy_accepts_every_spelling passed\n";
}

// --- in-process pipeline and interlace writers -------------------------------

void test_package_build_accepts_every_output_spelling(
    const fs::path& root, const PipelineInputs& inputs) {
  std::optional<int> reference_exit;
  for (const auto& spelling : output_spellings()) {
    const fs::path cwd = root / ("package-" + spelling.label);
    ScopedCwd scoped(cwd);
    const fs::path output = spelling.make(cwd, "clip.svp");
    const auto result = run_package_build(inputs, output);
    if (result.failure != svp::builder::BuildPipelineFailure::none ||
        !fs::is_regular_file(output)) {
      std::cerr << spelling.label << ": package not written, exit="
                << result.exit_code << " " << result.error_message << "\n";
      std::abort();
    }
    // Every spelling must produce the same verdict as every other.
    if (!reference_exit) reference_exit = result.exit_code;
    assert(result.exit_code == *reference_exit);
  }
  std::cout << "  test_package_build_accepts_every_output_spelling passed\n";
}

void test_package_write_failure_fails_the_build(
    const fs::path& root, const PipelineInputs& inputs) {
  ReadOnlyDirectory locked(root / "package-readonly");
  const fs::path output = locked.path() / "clip.svp";
  const auto result = run_package_build(inputs, output);
  assert(result.exit_code == svp::builder::kBuildFailedExitCode);
  assert(result.failure == svp::builder::BuildPipelineFailure::package_write);
  assert(!fs::exists(output));
  std::cout << "  test_package_write_failure_fails_the_build passed\n";
}

void test_svpi_create_accepts_every_output_spelling(
    const fs::path& root, const PipelineInputs& inputs) {
  for (const auto& spelling : output_spellings()) {
    const fs::path cwd = root / ("svpi-" + spelling.label);
    ScopedCwd scoped(cwd);
    const fs::path output = spelling.make(cwd, "clip.svpi");
    const auto result = run_core_only_svpi(inputs, output);
    if (!result.success || !fs::is_regular_file(output)) {
      std::cerr << spelling.label << ": svpi not written: "
                << result.error_message << "\n";
      std::abort();
    }
  }
  std::cout << "  test_svpi_create_accepts_every_output_spelling passed\n";
}

void test_svpi_write_failure_fails_create(const fs::path& root,
                                          const PipelineInputs& inputs) {
  ReadOnlyDirectory locked(root / "svpi-readonly");
  const fs::path output = locked.path() / "clip.svpi";
  const auto result = run_core_only_svpi(inputs, output);
  assert(!result.success);
  assert(!fs::exists(output));
  std::cout << "  test_svpi_write_failure_fails_create passed\n";
}

// --- command line -----------------------------------------------------------

std::vector<std::string> build_args(const PipelineInputs& inputs,
                                    const fs::path& output) {
  return {"build", inputs.media.string(),
          "--probe-json", inputs.probe.string(),
          "--ffmpeg", "/usr/bin/true",
          "--model-cache", inputs.model_cache.string(),
          "--force-single-speaker",
          "--progress", "none",
          "--out", output.string()};
}

std::vector<std::string> interlace_create_args(const PipelineInputs& inputs,
                                               const fs::path& output) {
  return {"interlace", "create", inputs.media.string(),
          "--ffprobe", "/usr/bin/true",
          "--core-only-diagnostic",
          "--progress", "none",
          "--out", output.string()};
}

void test_cli_build_accepts_every_output_spelling(
    const fs::path& builder, const fs::path& root,
    const PipelineInputs& inputs) {
  std::optional<int> reference_exit;
  for (const auto& spelling : output_spellings()) {
    const fs::path cwd = root / ("cli-build-" + spelling.label);
    ScopedCwd scoped(cwd);
    const fs::path output = spelling.make(cwd, "clip.svp");
    const auto result =
        run_builder(builder, build_args(inputs, output), root / "capture");
    if (!fs::is_regular_file(output)) {
      fail(spelling.label + ": build did not write the package", result);
    }
    if (result.err.find("create_directories") != std::string::npos) {
      fail(spelling.label + ": empty parent directory was created", result);
    }
    if (!reference_exit) reference_exit = result.exit_code;
    if (result.exit_code != *reference_exit) {
      fail(spelling.label + ": exit differs between spellings", result);
    }
    if (printed_success(result) != (result.exit_code == 0)) {
      fail(spelling.label + ": success line disagrees with exit", result);
    }
  }
  std::cout << "  test_cli_build_accepts_every_output_spelling passed\n";
}

void test_cli_build_write_failure_exits_nonzero_silently(
    const fs::path& builder, const fs::path& root,
    const PipelineInputs& inputs) {
  ReadOnlyDirectory locked(root / "cli-build-readonly");
  const fs::path output = locked.path() / "clip.svp";
  const auto result =
      run_builder(builder, build_args(inputs, output), root / "capture");
  if (result.exit_code == 0 || printed_success(result) ||
      fs::exists(output) ||
      result.err.find("failed to write SVP package") == std::string::npos) {
    fail("unwritable build output must fail without a success line", result);
  }
  std::cout << "  test_cli_build_write_failure_exits_nonzero_silently passed\n";
}

void test_cli_svpi_accepts_every_output_spelling(
    const fs::path& builder, const fs::path& root,
    const PipelineInputs& inputs) {
  for (const auto& spelling : output_spellings()) {
    const fs::path cwd = root / ("cli-svpi-" + spelling.label);
    ScopedCwd scoped(cwd);
    const fs::path output = spelling.make(cwd, "clip.svpi");
    const auto result = run_builder(
        builder, interlace_create_args(inputs, output), root / "capture");
    if (result.exit_code != 0 || !printed_success(result) ||
        !fs::is_regular_file(output)) {
      fail(spelling.label + ": interlace create failed", result);
    }
  }
  std::cout << "  test_cli_svpi_accepts_every_output_spelling passed\n";
}

void test_cli_svpi_write_failure_exits_nonzero_silently(
    const fs::path& builder, const fs::path& root,
    const PipelineInputs& inputs) {
  ReadOnlyDirectory locked(root / "cli-svpi-readonly");
  const fs::path output = locked.path() / "clip.svpi";
  const auto result = run_builder(
      builder, interlace_create_args(inputs, output), root / "capture");
  if (result.exit_code == 0 || printed_success(result) || fs::exists(output)) {
    fail("unwritable svpi output must fail without a success line", result);
  }
  std::cout << "  test_cli_svpi_write_failure_exits_nonzero_silently passed\n";
}

void test_cli_run_report_accepts_bare_filename(const fs::path& builder,
                                               const fs::path& root,
                                               const PipelineInputs& inputs) {
  const fs::path cwd = root / "cli-report-bare";
  ScopedCwd scoped(cwd);
  auto args = interlace_create_args(inputs, "clip.svpi");
  args.insert(args.end(), {"--run-report", "report.json"});
  const auto result = run_builder(builder, args, root / "capture");
  if (result.exit_code != 0 || !printed_success(result) ||
      !fs::is_regular_file(cwd / "report.json")) {
    fail("bare --run-report filename must be written in the cwd", result);
  }
  std::cout << "  test_cli_run_report_accepts_bare_filename passed\n";
}

void test_cli_run_report_write_failure_fails_command(
    const fs::path& builder, const fs::path& root,
    const PipelineInputs& inputs) {
  const fs::path cwd = root / "cli-report-readonly";
  ScopedCwd scoped(cwd);
  ReadOnlyDirectory locked(cwd / "locked");
  auto args = interlace_create_args(inputs, "clip.svpi");
  args.insert(args.end(),
              {"--run-report", (locked.path() / "report.json").string()});
  const auto result = run_builder(builder, args, root / "capture");
  if (result.exit_code == 0 || printed_success(result)) {
    fail("an unwritable --run-report must fail the command", result);
  }
  std::cout << "  test_cli_run_report_write_failure_fails_command passed\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: svp-builder-output-path-tests <svp-builder>\n";
    return 2;
  }
  const fs::path builder = fs::absolute(argv[1]);
  const fs::path root = make_root();
  const PipelineInputs inputs = write_pipeline_inputs(root);

  std::cout << "Running output path tests...\n";
  test_parent_directory_policy_accepts_every_spelling(root);
  test_package_build_accepts_every_output_spelling(root, inputs);
  test_package_write_failure_fails_the_build(root, inputs);
  test_svpi_create_accepts_every_output_spelling(root, inputs);
  test_svpi_write_failure_fails_create(root, inputs);
  test_cli_build_accepts_every_output_spelling(builder, root, inputs);
  test_cli_build_write_failure_exits_nonzero_silently(builder, root, inputs);
  test_cli_svpi_accepts_every_output_spelling(builder, root, inputs);
  test_cli_svpi_write_failure_exits_nonzero_silently(builder, root, inputs);
  test_cli_run_report_accepts_bare_filename(builder, root, inputs);
  test_cli_run_report_write_failure_fails_command(builder, root, inputs);

  fs::remove_all(root);
  std::cout << "All output path tests passed.\n";
  return 0;
}
