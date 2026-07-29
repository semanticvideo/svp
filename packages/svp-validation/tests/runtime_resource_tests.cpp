#include "svp/core/executable_path.hpp"
#include "svp/validation/report.hpp"
#include "svp/validation/runtime_resources.hpp"
#include "svp/validation/validator.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <system_error>
#include <utility>

#include <unistd.h>

namespace {

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                       \
      std::cerr << "CHECK failed: " #condition " at " << __FILE__ << ":"      \
                << __LINE__ << "\n";                                         \
      std::abort();                                                           \
    }                                                                         \
  } while (false)

class TemporaryDirectory {
 public:
  explicit TemporaryDirectory(std::string name)
      : path_(std::filesystem::temp_directory_path() /
              (std::move(name) + "-" + std::to_string(getpid()))) {
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

class CurrentDirectoryGuard {
 public:
  CurrentDirectoryGuard()
      : original_(std::filesystem::current_path()) {}
  ~CurrentDirectoryGuard() {
    std::error_code error;
    std::filesystem::current_path(original_, error);
  }

 private:
  std::filesystem::path original_;
};

std::filesystem::path copy_test_executable(
    const std::filesystem::path& destination) {
  std::filesystem::create_directories(destination.parent_path());
  std::filesystem::copy_file(svp::core::resolve_current_executable(), destination);
  std::filesystem::permissions(
      destination,
      std::filesystem::perms::owner_exec |
          std::filesystem::perms::group_exec |
          std::filesystem::perms::others_exec,
      std::filesystem::perm_options::add);
  return destination;
}

void copy_validation_codes(const std::filesystem::path& resource_root) {
  const auto registry_root = resource_root / "registries";
  std::filesystem::create_directories(registry_root);
  std::filesystem::create_directories(resource_root / "schemas");
  std::filesystem::copy_file(
      std::filesystem::path(SVP_SOURCE_DIR) / "spec" / "registries" /
          "validation-codes.json",
      registry_root / "validation-codes.json");
}

std::string read_bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::set<std::filesystem::path> relative_files(
    const std::filesystem::path& root) {
  std::set<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    if (entry.is_regular_file()) {
      files.insert(entry.path().lexically_relative(root));
    }
  }
  return files;
}

void test_installed_layout_and_symlink_resolution() {
  TemporaryDirectory temporary("svp-runtime-installed-layout");
  const auto executable =
      copy_test_executable(temporary.path() / "prefix" / "bin" / "tool");
  const auto resource_root = temporary.path() / "prefix" / "share" / "svp";
  copy_validation_codes(resource_root);

  const auto resources =
      svp::validation::resolve_default_runtime_resource_paths(executable);
  const auto canonical_resource_root =
      std::filesystem::weakly_canonical(resource_root);
  CHECK(resources.registry_root_path == canonical_resource_root / "registries");
  CHECK(resources.schema_root_path == canonical_resource_root / "schemas");

  const auto symlink = temporary.path() / "links" / "tool";
  std::filesystem::create_directories(symlink.parent_path());
  std::filesystem::create_symlink(executable, symlink);
  const auto symlink_resources =
      svp::validation::resolve_default_runtime_resource_paths(symlink);
  CHECK(symlink_resources.registry_root_path ==
        canonical_resource_root / "registries");
}

void test_multi_configuration_layout() {
  TemporaryDirectory temporary("svp-runtime-multi-config-layout");
  const auto executable = copy_test_executable(
      temporary.path() / "prefix" / "bin" / "Debug" / "tool");
  const auto resource_root = temporary.path() / "prefix" / "share" / "svp";
  copy_validation_codes(resource_root);

  const auto resources =
      svp::validation::resolve_default_runtime_resource_paths(executable);
  CHECK(resources.registry_root_path ==
        std::filesystem::weakly_canonical(resource_root) / "registries");
  CHECK(resources.attempted_resource_roots.size() == 2);
}

void test_explicit_override_precedence_and_derivation() {
  TemporaryDirectory temporary("svp-runtime-explicit-override");
  const auto executable =
      copy_test_executable(temporary.path() / "prefix" / "bin" / "tool");
  copy_validation_codes(temporary.path() / "prefix" / "share" / "svp");
  const auto explicit_codes = temporary.path() / "custom" / "registries" /
                              "validation-codes.json";
  copy_validation_codes(temporary.path() / "custom");

  const auto resources = svp::validation::resolve_validation_resource_paths(
      explicit_codes, {}, {}, executable);
  CHECK(resources.validation_codes_path == explicit_codes);
  CHECK(resources.registry_root_path == explicit_codes.parent_path());
  CHECK(resources.schema_root_path == temporary.path() / "custom" / "schemas");
  CHECK(resources.attempted_resource_roots.empty());
}

void test_explicit_root_precedence() {
  TemporaryDirectory temporary("svp-runtime-explicit-roots");
  const auto explicit_codes = temporary.path() / "codes" / "validation.json";
  const auto registry_root = temporary.path() / "registry-root";
  const auto schema_root = temporary.path() / "schema-root";
  const auto resources = svp::validation::resolve_validation_resource_paths(
      explicit_codes, registry_root, schema_root);
  CHECK(resources.validation_codes_path == explicit_codes);
  CHECK(resources.registry_root_path == registry_root);
  CHECK(resources.schema_root_path == schema_root);
}

void test_invalid_explicit_override_does_not_fall_back() {
  TemporaryDirectory temporary("svp-runtime-invalid-override");
  const auto invalid_codes = temporary.path() / "missing.json";
  svp::validation::ValidatorOptions options;
  options.validation_codes_path = invalid_codes;
  const auto report = svp::validation::validate_package(
      std::filesystem::path(SVP_VALID_FIXTURE), options);
  CHECK(report.status == svp::validation::ValidationStatus::unreadable);
  CHECK(!report.errors.empty());
  CHECK(report.errors.front().path == invalid_codes.string());
  CHECK(report.errors.front().message.find(invalid_codes.string()) !=
        std::string::npos);
}

void test_missing_default_resources_reports_every_candidate() {
  TemporaryDirectory temporary("svp-runtime-missing-default");
  const auto executable =
      copy_test_executable(temporary.path() / "prefix" / "bin" / "tool");
  try {
    (void)svp::validation::resolve_default_runtime_resource_paths(executable);
    CHECK(false);
  } catch (const std::runtime_error& error) {
    const std::string message = error.what();
    const auto executable_directory =
        svp::core::resolve_executable(executable).parent_path();
    const auto first =
        (executable_directory / ".." / "share" / "svp").lexically_normal();
    const auto second =
        (executable_directory / ".." / ".." / "share" / "svp")
            .lexically_normal();
    CHECK(message.find("SVP runtime resources could not be located") !=
          std::string::npos);
    CHECK(message.find(first.string()) != std::string::npos);
    CHECK(message.find(
              (first / "registries" / "validation-codes.json").string()) !=
          std::string::npos);
    CHECK(message.find(second.string()) != std::string::npos);
    CHECK(message.find(
              (second / "registries" / "validation-codes.json").string()) !=
          std::string::npos);
    CHECK(message.find("--validation-codes <path>") != std::string::npos);
  }
}

void test_default_discovery_is_cwd_independent() {
  TemporaryDirectory temporary("svp-runtime-unrelated-cwd");
  CurrentDirectoryGuard guard;
  std::filesystem::current_path(temporary.path());
  const auto report = svp::validation::validate_package(
      std::filesystem::path(SVP_VALID_FIXTURE), {});
  CHECK(report.status != svp::validation::ValidationStatus::unreadable);
}

void test_staged_resources_are_complete_and_byte_identical() {
  const std::filesystem::path source_root(SVP_SOURCE_DIR);
  const std::filesystem::path staged_root(SVP_RUNTIME_RESOURCE_DIR);
  for (const auto& directory : {"registries", "schemas"}) {
    const auto source = source_root / "spec" / directory;
    const auto staged = staged_root / directory;
    const auto source_files = relative_files(source);
    const auto staged_files = relative_files(staged);
    CHECK(source_files == staged_files);
    for (const auto& relative : source_files) {
      CHECK(read_bytes(source / relative) == read_bytes(staged / relative));
    }
  }
}

}  // namespace

int main() {
  test_installed_layout_and_symlink_resolution();
  test_multi_configuration_layout();
  test_explicit_override_precedence_and_derivation();
  test_explicit_root_precedence();
  test_invalid_explicit_override_does_not_fall_back();
  test_missing_default_resources_reports_every_candidate();
  test_default_discovery_is_cwd_independent();
  test_staged_resources_are_complete_and_byte_identical();
  return 0;
}
