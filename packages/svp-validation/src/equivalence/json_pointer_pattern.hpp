#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <string_view>

namespace svp::validation::equivalence {

// Visits every value addressed by a JSON Pointer pattern. A "*" segment
// matches every array index or object key. Registry patterns use plain key
// names, so "~0"/"~1" escapes are not interpreted. The callback receives the
// concrete pointer of the match.
void for_each_pointer_match(
    nlohmann::json& document,
    std::string_view pattern,
    const std::function<void(nlohmann::json& value, const std::string& pointer)>& visit);

[[nodiscard]] const nlohmann::json* find_pointer(const nlohmann::json& document,
                                                 std::string_view pointer);

}  // namespace svp::validation::equivalence
