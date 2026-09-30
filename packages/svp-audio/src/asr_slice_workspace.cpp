#include "svp/audio/asr_slice_workspace.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

namespace svp::audio {
namespace {

// mkdtemp(3) requires the template to end in exactly this placeholder.
constexpr std::string_view kMkdtempPlaceholder = "XXXXXX";

std::filesystem::path create_unique_directory(
    const std::filesystem::path& parent_dir) {
  std::filesystem::create_directories(parent_dir);
  const std::string pattern =
      (parent_dir / (std::string(kAsrSliceWorkspacePrefix) +
                     std::string(kMkdtempPlaceholder)))
          .string();
  std::vector<char> buffer(pattern.begin(), pattern.end());
  buffer.push_back('\0');
  if (::mkdtemp(buffer.data()) == nullptr) {
    throw std::runtime_error("unable to create ASR slice workspace under " +
                             parent_dir.string() + ": " +
                             std::strerror(errno));
  }
  return std::filesystem::path(buffer.data());
}

}  // namespace

AsrSliceWorkspace::AsrSliceWorkspace()
    : AsrSliceWorkspace(std::filesystem::temp_directory_path()) {}

AsrSliceWorkspace::AsrSliceWorkspace(const std::filesystem::path& parent_dir)
    : path_(create_unique_directory(parent_dir)) {}

AsrSliceWorkspace::~AsrSliceWorkspace() {
  std::error_code ignored;
  std::filesystem::remove_all(path_, ignored);
}

}  // namespace svp::audio
