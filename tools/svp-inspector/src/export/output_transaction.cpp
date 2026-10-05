#include "output_transaction.hpp"

#include "export_error.hpp"
#include "export_plan.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <system_error>
#include <utility>
#include <vector>

namespace package_export {
namespace {

// I/O batching only: how many bytes a StagedFile collects before one write(2).
// It does not change what is written.
constexpr std::size_t kWriteBufferBytes = std::size_t{1} << 20U;

// Prefixes of the hidden sibling directories used while publishing.
constexpr std::string_view kStagingInfix = ".svp-export-staging-";
constexpr std::string_view kReplacedInfix = ".svp-export-replaced-";
// mkdtemp(3) replaces this suffix with a unique name.
constexpr std::string_view kUniqueSuffix = "XXXXXX";

[[noreturn]] void throw_write_failed(const std::string& what, int error) {
  throw ExportError(ExportErrorCode::output_write_failed,
                    what + ": " + std::strerror(error));
}

std::filesystem::path normalized_target(const std::filesystem::path& out) {
  auto target = out.lexically_normal();
  if (target.filename().empty() && target.has_parent_path()) {
    target = target.parent_path();
  }
  return target;
}

std::filesystem::path parent_of(const std::filesystem::path& target) {
  const auto parent = target.parent_path();
  return parent.empty() ? std::filesystem::path{"."} : parent;
}

std::filesystem::path make_unique_sibling(const std::filesystem::path& target,
                                          std::string_view infix) {
  const auto name = "." + target.filename().string() + std::string{infix} +
                    std::string{kUniqueSuffix};
  auto pattern = (parent_of(target) / name).string();
  std::vector<char> buffer(pattern.begin(), pattern.end());
  buffer.push_back('\0');
  if (::mkdtemp(buffer.data()) == nullptr) {
    throw_write_failed("Could not create a staging directory next to --out",
                       errno);
  }
  return std::filesystem::path{buffer.data()};
}

void apply_default_directory_mode(const std::filesystem::path& directory) {
  // mkdtemp creates 0700; give the export the mode mkdir(2) would.
  const mode_t mask = ::umask(0);
  ::umask(mask);
  ::chmod(directory.c_str(), static_cast<mode_t>(0777) & ~mask);
}

bool is_empty_directory(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::is_empty(path, error) && !error;
}

bool holds_previous_export(const std::filesystem::path& directory) {
  std::error_code error;
  const auto summary = std::filesystem::symlink_status(
      directory / std::string{kSummaryFileName}, error);
  return !error && std::filesystem::is_regular_file(summary);
}

void remove_tree_quietly(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove_all(path, ignored);
}

}  // namespace

StagedFile::StagedFile(int descriptor, std::string relative_path)
    : descriptor_(descriptor), relative_path_(std::move(relative_path)) {
  buffer_.reserve(kWriteBufferBytes);
}

StagedFile::StagedFile(StagedFile&& other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1)),
      relative_path_(std::move(other.relative_path_)),
      buffer_(std::move(other.buffer_)),
      size_(other.size_) {}

StagedFile::~StagedFile() {
  if (descriptor_ >= 0) {
    ::close(descriptor_);
  }
}

void StagedFile::write(std::string_view bytes) {
  size_ += bytes.size();
  if (buffer_.size() + bytes.size() > kWriteBufferBytes) {
    flush();
  }
  if (bytes.size() >= kWriteBufferBytes) {
    buffer_.assign(bytes);
    flush();
    return;
  }
  buffer_.append(bytes);
}

void StagedFile::flush() {
  std::size_t written = 0;
  while (written < buffer_.size()) {
    const auto count = ::write(descriptor_, buffer_.data() + written,
                               buffer_.size() - written);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_write_failed("Could not write " + relative_path_, errno);
    }
    written += static_cast<std::size_t>(count);
  }
  buffer_.clear();
}

void StagedFile::close() {
  flush();
  const int descriptor = std::exchange(descriptor_, -1);
  if (::close(descriptor) != 0) {
    throw_write_failed("Could not close " + relative_path_, errno);
  }
}

void OutputTransaction::check_target(const std::filesystem::path& out,
                                     bool overwrite) {
  const auto target = normalized_target(out);
  std::error_code error;
  const auto status = std::filesystem::symlink_status(target, error);
  if (error || !std::filesystem::exists(status)) {
    return;
  }
  const nlohmann::json details{{"out", out.string()}};
  if (std::filesystem::is_symlink(status)) {
    throw ExportError(ExportErrorCode::output_not_replaceable,
                      "--out is a symbolic link; the export only writes to a "
                      "real directory path.",
                      details);
  }
  if (std::filesystem::is_directory(status)) {
    if (is_empty_directory(target)) {
      return;
    }
    if (!overwrite) {
      throw ExportError(ExportErrorCode::output_exists,
                        "--out exists and is not empty; pass --overwrite to "
                        "replace a previous export.",
                        details);
    }
    if (!holds_previous_export(target)) {
      throw ExportError(ExportErrorCode::output_not_replaceable,
                        "--overwrite only replaces a previous export (a "
                        "directory holding export.json).",
                        details);
    }
    return;
  }
  throw ExportError(overwrite ? ExportErrorCode::output_not_replaceable
                              : ExportErrorCode::output_exists,
                    "--out exists and is not a directory.", details);
}

void OutputTransaction::check_input_outside(const std::filesystem::path& input,
                                            const std::filesystem::path& out) {
  std::error_code error;
  const auto target = std::filesystem::weakly_canonical(normalized_target(out), error);
  if (error) {
    return;
  }
  const auto package = std::filesystem::weakly_canonical(input, error);
  if (error) {
    return;
  }
  const auto [target_end, package_position] =
      std::mismatch(target.begin(), target.end(), package.begin(), package.end());
  if (target_end == target.end()) {
    throw ExportError(ExportErrorCode::output_not_replaceable,
                      "--out contains the input package; choose an output "
                      "directory outside it.",
                      nlohmann::json{{"out", out.string()}});
  }
}

void OutputTransaction::check_free_space(const std::filesystem::path& out,
                                         std::uint64_t required_bytes) {
  const auto target = normalized_target(out);
  // The nearest existing ancestor is on the file system that will hold --out.
  auto probe = parent_of(target);
  std::error_code error;
  while (!std::filesystem::exists(probe, error) && probe.has_parent_path() &&
         probe != probe.parent_path()) {
    probe = probe.parent_path();
  }
  const auto space = std::filesystem::space(probe, error);
  if (error) {
    return;  // Unknown free space is not a reason to refuse.
  }
  if (space.available < required_bytes) {
    throw ExportError(ExportErrorCode::insufficient_space,
                      "The file system holding --out has less free space than "
                      "the package's declared content.",
                      nlohmann::json{{"required_bytes", required_bytes},
                                     {"available_bytes", space.available}});
  }
}

OutputTransaction::OutputTransaction(const std::filesystem::path& out,
                                     bool overwrite)
    : target_(normalized_target(out)), overwrite_(overwrite) {
  std::error_code error;
  std::filesystem::create_directories(parent_of(target_), error);
  if (error) {
    throw_write_failed("Could not create the parent directory of --out",
                       error.value());
  }
  staging_ = make_unique_sibling(target_, kStagingInfix);
  apply_default_directory_mode(staging_);
}

OutputTransaction::~OutputTransaction() {
  if (!published_) {
    remove_tree_quietly(staging_);
  }
}

void OutputTransaction::ensure_directory(std::string_view relative_directory) {
  if (relative_directory.empty() ||
      created_directories_.contains(relative_directory)) {
    return;
  }
  const auto slash = relative_directory.rfind('/');
  if (slash != std::string_view::npos) {
    ensure_directory(relative_directory.substr(0, slash));
  }
  const auto path = staging_ / std::string{relative_directory};
  if (::mkdir(path.c_str(), 0777) != 0 && errno != EEXIST) {
    throw_write_failed("Could not create directory " +
                           std::string{relative_directory},
                       errno);
  }
  struct stat status {};
  if (::lstat(path.c_str(), &status) != 0 || !S_ISDIR(status.st_mode)) {
    throw ExportError(ExportErrorCode::output_path_collision,
                      "An exported directory collides with an exported file.",
                      nlohmann::json{{"path", std::string{relative_directory}}});
  }
  created_directories_.emplace(relative_directory);
}

StagedFile OutputTransaction::create_file(std::string_view relative_path) {
  const auto slash = relative_path.rfind('/');
  if (slash != std::string_view::npos) {
    ensure_directory(relative_path.substr(0, slash));
  }
  const auto path = staging_ / std::string{relative_path};
  const int descriptor =
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
             0666);
  if (descriptor < 0) {
    const int error = errno;
    if (error == EEXIST || error == EISDIR || error == ENOTDIR ||
        error == ELOOP) {
      throw ExportError(ExportErrorCode::output_path_collision,
                        "An exported file collides with another exported path.",
                        nlohmann::json{{"path", std::string{relative_path}}});
    }
    throw_write_failed("Could not create " + std::string{relative_path},
                       error);
  }
  return StagedFile{descriptor, std::string{relative_path}};
}

void OutputTransaction::publish() {
  // --out may have changed since the first check; apply the same rules again.
  check_target(target_, overwrite_);

  std::error_code error;
  const auto status = std::filesystem::symlink_status(target_, error);
  const bool replace_previous = !error && std::filesystem::exists(status) &&
                                !is_empty_directory(target_);
  if (!replace_previous) {
    if (::rename(staging_.c_str(), target_.c_str()) != 0) {
      throw_write_failed("Could not move the export into --out", errno);
    }
    published_ = true;
    return;
  }

  const auto replaced = make_unique_sibling(target_, kReplacedInfix);
  if (::rename(target_.c_str(), replaced.c_str()) != 0) {
    const int rename_error = errno;
    remove_tree_quietly(replaced);
    throw_write_failed("Could not move the previous export aside",
                       rename_error);
  }
  if (::rename(staging_.c_str(), target_.c_str()) != 0) {
    const int rename_error = errno;
    ::rename(replaced.c_str(), target_.c_str());
    throw_write_failed("Could not move the export into --out", rename_error);
  }
  published_ = true;
  remove_tree_quietly(replaced);
}

}  // namespace package_export
