#pragma once

// Strict JSON field access for the audio task parameter schemas: unknown,
// missing, or mistyped fields are std::invalid_argument naming the field.

#include "svp/models/thread_plan.hpp"

#include <nlohmann/json.hpp>

#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace svp::audio::tasks::detail {

inline void require_only(const nlohmann::json& value, std::initializer_list<std::string_view> keys,
                         const std::string& where) {
  if (!value.is_object()) {
    throw std::invalid_argument(where + " must be an object");
  }
  for (const auto& [key, field] : value.items()) {
    bool known = false;
    for (const std::string_view allowed : keys) {
      known = known || key == allowed;
    }
    if (!known) {
      throw std::invalid_argument(where + " has unknown field `" + key + "`");
    }
  }
  for (const std::string_view key : keys) {
    if (!value.contains(std::string(key))) {
      throw std::invalid_argument(where + " is missing `" + std::string(key) + "`");
    }
  }
}

template <typename T>
T required(const nlohmann::json& value, std::string_view key, const std::string& where) {
  const nlohmann::json& field = value.at(std::string(key));
  if constexpr (std::is_same_v<T, std::string>) {
    if (!field.is_string()) {
      throw std::invalid_argument(where + "." + std::string(key) + " must be a string");
    }
  } else if constexpr (std::is_same_v<T, bool>) {
    if (!field.is_boolean()) {
      throw std::invalid_argument(where + "." + std::string(key) + " must be a boolean");
    }
  } else if constexpr (std::is_unsigned_v<T>) {
    if (!field.is_number_unsigned()) {
      throw std::invalid_argument(where + "." + std::string(key) +
                                  " must be a non-negative integer");
    }
  } else {
    if (!field.is_number_integer()) {
      throw std::invalid_argument(where + "." + std::string(key) + " must be an integer");
    }
  }
  return field.get<T>();
}

inline nlohmann::json ort_threads_json(const svp::models::OrtThreadCounts& threads) {
  return {{"inter_op", threads.inter_op}, {"intra_op", threads.intra_op}};
}

inline svp::models::OrtThreadCounts ort_threads_from_json(const nlohmann::json& value,
                                                          const std::string& where) {
  require_only(value, {"inter_op", "intra_op"}, where);
  svp::models::OrtThreadCounts threads;
  threads.inter_op = required<int>(value, "inter_op", where);
  threads.intra_op = required<int>(value, "intra_op", where);
  if (threads.inter_op < 1 || threads.intra_op < 1) {
    throw std::invalid_argument(where + ": thread counts must be at least 1");
  }
  return threads;
}

}  // namespace svp::audio::tasks::detail
