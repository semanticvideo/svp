#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace svp::audio {

class WhisperTokenTable {
 public:
  [[nodiscard]] bool load(const std::filesystem::path& tokens_path);

  [[nodiscard]] std::optional<std::string> token_text(int token_id) const;

  [[nodiscard]] std::size_t size() const noexcept;

  static constexpr int kEot = 50256;
  static constexpr int kSot = 50257;

 private:
  std::vector<std::string> tokens_;
};

}  // namespace svp::audio
