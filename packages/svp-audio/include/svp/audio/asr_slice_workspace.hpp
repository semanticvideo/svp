#pragma once

#include <filesystem>
#include <string_view>

namespace svp::audio {

// Directory-name prefix for ASR chunk-slice workspaces. Owner: ASR execution
// boundary. The prefix only makes leftover directories recognizable in the
// system temp directory; uniqueness comes from the mkdtemp suffix, so two
// builds (or two boundary executions in one process) never share slice paths.
inline constexpr std::string_view kAsrSliceWorkspacePrefix =
    "svp-asr-chunk-slices-";

// Owns a freshly created, uniquely named scratch directory for the short-lived
// per-chunk WAV slices of one ASR boundary execution. The directory and any
// slices left in it are removed when the workspace is destroyed, including
// when inference throws.
class AsrSliceWorkspace {
 public:
  AsrSliceWorkspace();
  explicit AsrSliceWorkspace(const std::filesystem::path& parent_dir);
  ~AsrSliceWorkspace();

  AsrSliceWorkspace(const AsrSliceWorkspace&) = delete;
  AsrSliceWorkspace& operator=(const AsrSliceWorkspace&) = delete;
  AsrSliceWorkspace(AsrSliceWorkspace&&) = delete;
  AsrSliceWorkspace& operator=(AsrSliceWorkspace&&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace svp::audio
