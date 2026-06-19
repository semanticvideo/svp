#include "svp/models/error.hpp"

#include <utility>

namespace svp::models {

ModelError::ModelError(ModelErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

ModelErrorCode ModelError::code() const noexcept {
  return code_;
}

}  // namespace svp::models
