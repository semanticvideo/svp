#pragma once

#include <string>
#include <string_view>

namespace svp::core {

class Identifier {
 public:
  explicit Identifier(std::string value);

  [[nodiscard]] const std::string& value() const noexcept;
  [[nodiscard]] bool empty() const noexcept;

 private:
  std::string value_;
};

[[nodiscard]] bool is_portable_identifier(std::string_view value) noexcept;

}  // namespace svp::core

