#pragma once

#include <filesystem>
#include <string>

namespace svp::builder::detail {

class BindingMediaCandidate {
 public:
  BindingMediaCandidate() = default;
  ~BindingMediaCandidate();
  BindingMediaCandidate(const BindingMediaCandidate&) = delete;
  BindingMediaCandidate& operator=(const BindingMediaCandidate&) = delete;
  BindingMediaCandidate(BindingMediaCandidate&& other) noexcept;
  BindingMediaCandidate& operator=(BindingMediaCandidate&& other) noexcept;

  std::filesystem::path path;
  std::filesystem::path temporary_directory;
};

struct BindingMediaResult {
  bool success = false;
  BindingMediaCandidate candidate;
  std::string error_message;
};

[[nodiscard]] BindingMediaResult prepare_binding_media(
    const std::filesystem::path& media_path,
    bool replace_existing);

}  // namespace svp::builder::detail
