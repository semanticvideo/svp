#pragma once

// Vectors (float32 embeddings, uint16 depth fields) as task output bytes:
// the values back to back in little-endian order, exactly the in-memory
// layout of an Apple Silicon (arm64) runtime, the only hosts that run these
// tasks (plan §0: out of scope are Intel Macs and distributed Linux builds).

#include <bit>
#include <cstddef>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace svp::vision::tasks::detail {

static_assert(std::endian::native == std::endian::little,
              "dispatched vision task payloads are little-endian in memory");

template <typename Value>
std::vector<std::byte> values_to_bytes(const std::vector<Value>& values) {
  static_assert(std::is_trivially_copyable_v<Value>);
  std::vector<std::byte> bytes(values.size() * sizeof(Value));
  if (!bytes.empty()) {
    std::memcpy(bytes.data(), values.data(), bytes.size());
  }
  return bytes;
}

// Throws std::invalid_argument unless `bytes` holds exactly `count` values.
template <typename Value>
std::vector<Value> values_from_bytes(std::span<const std::byte> bytes, std::size_t count,
                                     const std::string& where) {
  static_assert(std::is_trivially_copyable_v<Value>);
  if (bytes.size() != count * sizeof(Value)) {
    throw std::invalid_argument(where + ": expected " + std::to_string(count) + " values, got " +
                                std::to_string(bytes.size()) + " bytes");
  }
  std::vector<Value> values(count);
  if (count > 0) {
    std::memcpy(values.data(), bytes.data(), bytes.size());
  }
  return values;
}

}  // namespace svp::vision::tasks::detail
