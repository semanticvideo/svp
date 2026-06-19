#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace svp::core {

enum class HashAlgorithm {
  blake3,
};

std::string_view hash_algorithm_name(HashAlgorithm algorithm) noexcept;

class HashString {
 public:
  HashString(HashAlgorithm algorithm, std::string hex_value);

  [[nodiscard]] HashAlgorithm algorithm() const noexcept;
  [[nodiscard]] const std::string& hex_value() const noexcept;
  [[nodiscard]] std::string canonical() const;

 private:
  HashAlgorithm algorithm_;
  std::string hex_value_;
};

[[nodiscard]] bool is_lower_hex(std::string_view value) noexcept;
[[nodiscard]] std::optional<HashString> parse_hash_string(std::string_view value);

}  // namespace svp::core

