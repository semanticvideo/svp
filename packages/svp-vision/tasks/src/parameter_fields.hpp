#pragma once

// Strict reading of task parameter objects (plan §4.3: a runtime validates
// parameters against each type's schema). Every vision task type's codec
// reads its fields through these, so unknown, missing, or mistyped fields
// and out-of-range values are rejected the same way for every type, with
// "<task type> parameters: <reason>" messages.

#include "svp/models/thread_plan.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace svp::vision::tasks::detail {

using Json = nlohmann::json;

// ONNX Runtime execution providers svp::models::OnnxSession can create.
inline constexpr std::array<std::string_view, 2> kExecutionProviders = {"cpu", "coreml"};

class ParameterFields {
 public:
  explicit ParameterFields(std::string_view task_type)
      : prefix_(std::string(task_type) + " parameters: ") {}

  [[noreturn]] void reject(const std::string& message) const {
    throw std::invalid_argument(prefix_ + message);
  }

  void require_fields(const Json& value, const std::set<std::string>& expected,
                      const std::string& where) const {
    if (!value.is_object()) reject(where + " must be an object");
    for (const auto& [key, unused] : value.items()) {
      if (!expected.contains(key)) reject(where + " has unknown field `" + key + "`");
    }
    for (const std::string& key : expected) {
      if (!value.contains(key)) reject(where + " is missing field `" + key + "`");
    }
  }

  template <typename Integer>
  Integer integer_at(const Json& object, const std::string& key, const std::string& where,
                     Integer minimum,
                     Integer maximum = std::numeric_limits<Integer>::max()) const {
    const Json& value = object.at(key);
    const std::string name = where + "." + key;
    if (!value.is_number_integer()) reject(name + " must be an integer");
    Integer parsed{};
    if (value.is_number_unsigned()) {
      const auto raw = value.get<std::uint64_t>();
      if (raw > static_cast<std::uint64_t>(std::numeric_limits<Integer>::max())) {
        reject(name + " is out of range");
      }
      parsed = static_cast<Integer>(raw);
    } else {
      const auto raw = value.get<std::int64_t>();
      if constexpr (std::is_unsigned_v<Integer>) {
        reject(name + " must not be negative");
      } else {
        if (raw < static_cast<std::int64_t>(std::numeric_limits<Integer>::min()) ||
            raw > static_cast<std::int64_t>(std::numeric_limits<Integer>::max())) {
          reject(name + " is out of range");
        }
        parsed = static_cast<Integer>(raw);
      }
    }
    if (parsed < minimum || parsed > maximum) {
      reject(name + " must be in [" + std::to_string(minimum) + ", " +
             std::to_string(maximum) + "]");
    }
    return parsed;
  }

  double double_at(const Json& object, const std::string& key, const std::string& where,
                   double minimum, double maximum) const;

  std::string string_at(const Json& object, const std::string& key,
                        const std::string& where) const {
    const Json& value = object.at(key);
    if (!value.is_string()) reject(where + "." + key + " must be a string");
    return value.get<std::string>();
  }

  bool bool_at(const Json& object, const std::string& key, const std::string& where) const {
    const Json& value = object.at(key);
    if (!value.is_boolean()) reject(where + "." + key + " must be a boolean");
    return value.get<bool>();
  }

  template <std::size_t N>
  std::string one_of(const Json& object, const std::string& key, const std::string& where,
                      const std::array<std::string_view, N>& allowed) const {
    const std::string value = string_at(object, key, where);
    if (std::find(allowed.begin(), allowed.end(), value) == allowed.end()) {
      reject(where + "." + key + " `" + value + "` is not supported");
    }
    return value;
  }

  // A canonical model id (svp::models::is_canonical_model_id).
  std::string model_id_at(const Json& object, const std::string& where) const;

  // Thread counts must be explicit: kRuntimeChoosesThreadCount (0) would let
  // each worker size its pools from its own host, and ONNX Runtime thread
  // counts can change serialized output (plan §2.4 item 5).
  svp::models::OrtThreadCounts threads_at(const Json& object, const std::string& where) const;

  // "b3:<64 hex>", the ffmpeg_build_identity() every executor must decode
  // with.
  std::string ffmpeg_build_at(const Json& object, const std::string& where) const;

  // A JSON array field.
  const Json& array_at(const Json& object, const std::string& key,
                       const std::string& where) const {
    const Json& value = object.at(key);
    if (!value.is_array()) reject(where + "." + key + " must be an array");
    return value;
  }

 private:
  std::string prefix_;
};

[[nodiscard]] Json threads_to_json(const svp::models::OrtThreadCounts& threads);

}  // namespace svp::vision::tasks::detail
