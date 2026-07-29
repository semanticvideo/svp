#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

#include <sys/wait.h>
#include <unistd.h>

namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory()
      : path_(std::filesystem::temp_directory_path() /
              ("svp-validator-runtime-cwd-" + std::to_string(getpid()))) {
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

int run_validator(const std::filesystem::path& validator,
                  const std::filesystem::path& fixture,
                  const std::filesystem::path& working_directory,
                  const std::filesystem::path& validation_codes = {}) {
  const pid_t child = fork();
  if (child == 0) {
    if (chdir(working_directory.c_str()) != 0) {
      _exit(126);
    }
    if (validation_codes.empty()) {
      execl(validator.c_str(), validator.c_str(), "validate", fixture.c_str(),
            "--json", static_cast<char*>(nullptr));
    } else {
      execl(validator.c_str(), validator.c_str(), "validate", fixture.c_str(),
            "--json", "--validation-codes", validation_codes.c_str(),
            static_cast<char*>(nullptr));
    }
    _exit(127);
  }
  if (child < 0) {
    return 125;
  }

  int status = 0;
  if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status)) {
    return 124;
  }
  return WEXITSTATUS(status);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "expected validator, fixture, and validation-code paths\n";
    return 2;
  }

  const auto validator = std::filesystem::absolute(argv[1]);
  const auto fixture = std::filesystem::absolute(argv[2]);
  const auto validation_codes = std::filesystem::absolute(argv[3]);
  TemporaryDirectory temporary;

  const int discovered =
      run_validator(validator, fixture, temporary.path());
  if (discovered != 0 && discovered != 1) {
    std::cerr << "default executable-relative discovery failed with exit code "
              << discovered << "\n";
    return 1;
  }

  const int overridden = run_validator(
      validator, fixture, temporary.path(), validation_codes);
  if (overridden != 0 && overridden != 1) {
    std::cerr << "explicit --validation-codes failed with exit code "
              << overridden << "\n";
    return 1;
  }
  return 0;
}
