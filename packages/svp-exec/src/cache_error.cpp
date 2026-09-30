#include "svp/exec/cache_error.hpp"

#include "cache_error_mapping.hpp"

#include <cerrno>

namespace svp::exec {

std::string_view cache_error_code_name(CacheErrorCode code) noexcept {
  switch (code) {
    case CacheErrorCode::unavailable:
      return "unavailable";
    case CacheErrorCode::permission_denied:
      return "permission_denied";
    case CacheErrorCode::no_space:
      return "no_space";
    case CacheErrorCode::io_error:
      return "io_error";
    case CacheErrorCode::not_found:
      return "not_found";
    case CacheErrorCode::corrupt:
      return "corrupt";
    case CacheErrorCode::busy:
      return "busy";
    case CacheErrorCode::invalid_argument:
      return "invalid_argument";
  }
  return "unknown";
}

namespace detail {

CacheErrorCode cache_error_code_for(const std::error_code& error) noexcept {
  if (error == std::errc::permission_denied || error == std::errc::operation_not_permitted ||
      error == std::errc::read_only_file_system) {
    return CacheErrorCode::permission_denied;
  }
  if (error == std::errc::no_space_on_device) {
    return CacheErrorCode::no_space;
  }
#if defined(EDQUOT)
  if (error.value() == EDQUOT && error.category() == std::generic_category()) {
    return CacheErrorCode::no_space;
  }
#endif
  if (error == std::errc::no_such_file_or_directory) {
    return CacheErrorCode::not_found;
  }
  return CacheErrorCode::io_error;
}

CacheError cache_error(const std::error_code& error, std::string_view context) {
  return CacheError{.code = cache_error_code_for(error),
                    .message = std::string(context) + ": " + error.message()};
}

CacheError cache_error(CacheErrorCode code, std::string message) {
  return CacheError{.code = code, .message = std::move(message)};
}

}  // namespace detail
}  // namespace svp::exec
