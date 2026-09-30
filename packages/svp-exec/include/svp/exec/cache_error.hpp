#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace svp::exec {

// RC2 §20.5.2: cache read, write, permission, quota, corruption, or eviction
// failures MUST NOT fail the build. Every CasStore operation therefore returns
// one of these codes instead of throwing, and the caller falls back to direct
// compute and emits WARN_CACHE_DISABLED (or a more specific warning).
enum class CacheErrorCode {
  // The cache root cannot be resolved or created.
  unavailable,
  permission_denied,
  // The filesystem is full or a quota was reached.
  no_space,
  io_error,
  // No blob with that digest is stored.
  not_found,
  // A stored blob no longer hashes to its name. The blob has been removed.
  corrupt,
  // Another process holds the lock this operation needs.
  busy,
  invalid_argument,
};

[[nodiscard]] std::string_view cache_error_code_name(CacheErrorCode code) noexcept;

struct CacheError {
  CacheErrorCode code = CacheErrorCode::io_error;
  std::string message;
};

// Value-or-CacheError. C++20 has no std::expected; this is the minimal subset
// the cache needs.
template <typename T>
class [[nodiscard]] CacheResult {
 public:
  CacheResult(T value) : state_(std::move(value)) {}
  CacheResult(CacheError error) : state_(std::move(error)) {}

  [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(state_); }
  explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] T& value() & { return std::get<T>(state_); }
  [[nodiscard]] const T& value() const& { return std::get<T>(state_); }
  [[nodiscard]] T&& value() && { return std::get<T>(std::move(state_)); }
  [[nodiscard]] const CacheError& error() const { return std::get<CacheError>(state_); }

 private:
  std::variant<T, CacheError> state_;
};

struct CacheOk {};
using CacheStatus = CacheResult<CacheOk>;

}  // namespace svp::exec
