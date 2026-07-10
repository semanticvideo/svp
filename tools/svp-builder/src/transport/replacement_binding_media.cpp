#include "replacement_binding_media.hpp"

#include "svp/package/embedded_svpi.hpp"

#include <utility>
#include <vector>

#include <unistd.h>

namespace svp::builder::detail {

BindingMediaCandidate::~BindingMediaCandidate() {
  if (!temporary_directory.empty()) {
    std::error_code ignored;
    std::filesystem::remove_all(temporary_directory, ignored);
  }
}

BindingMediaCandidate::BindingMediaCandidate(
    BindingMediaCandidate&& other) noexcept
    : path(std::move(other.path)),
      temporary_directory(std::move(other.temporary_directory)) {
  other.temporary_directory.clear();
}

BindingMediaCandidate& BindingMediaCandidate::operator=(
    BindingMediaCandidate&& other) noexcept {
  if (this != &other) {
    if (!temporary_directory.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(temporary_directory, ignored);
    }
    path = std::move(other.path);
    temporary_directory = std::move(other.temporary_directory);
    other.temporary_directory.clear();
  }
  return *this;
}

BindingMediaResult prepare_binding_media(
    const std::filesystem::path& media_path,
    bool replace_existing) {
  BindingMediaResult result;
  const auto inspection = svp::package::inspect_embedded_svpi(media_path, false);
  if (inspection.embeddings.empty()) {
    result.success = true;
    result.candidate.path = media_path;
    return result;
  }
  if (!replace_existing) {
    result.error_message =
        "Container already contains embedded SVPI; use --replace-existing.";
    return result;
  }

  auto pattern = (std::filesystem::temp_directory_path() /
                  "svp-binding-media-XXXXXX").string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  if (mkdtemp(writable.data()) == nullptr) {
    result.error_message =
        "Unable to create temporary directory for replacement binding verification.";
    return result;
  }
  result.candidate.temporary_directory = writable.data();
  result.candidate.path = result.candidate.temporary_directory /
                          ("clean" + media_path.extension().string());
  const auto stripped = svp::package::strip_embedded_svpi(
      media_path, result.candidate.path);
  if (!stripped.success) {
    result.error_message =
        "Unable to reconstruct the clean container for binding verification: " +
        stripped.error_message;
    return result;
  }
  result.success = true;
  return result;
}

}  // namespace svp::builder::detail
