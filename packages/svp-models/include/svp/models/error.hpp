#pragma once

#include <stdexcept>
#include <string>

namespace svp::models {

enum class ModelErrorCode {
  io_error,
  schema_error,
  hash_mismatch,
  missing_model,
  runtime_unavailable,
};

class ModelError : public std::runtime_error {
 public:
  ModelError(ModelErrorCode code, std::string message);

  [[nodiscard]] ModelErrorCode code() const noexcept;

 private:
  ModelErrorCode code_;
};

}  // namespace svp::models
