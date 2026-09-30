#include "equivalence/json_pointer_pattern.hpp"

#include <vector>

namespace svp::validation::equivalence {
namespace {

constexpr std::string_view kWildcard = "*";

std::vector<std::string> split_pointer(std::string_view pointer) {
  std::vector<std::string> segments;
  if (pointer.empty()) {
    return segments;
  }
  std::size_t start = pointer.front() == '/' ? 1 : 0;
  while (start <= pointer.size()) {
    const auto slash = pointer.find('/', start);
    const auto end = slash == std::string_view::npos ? pointer.size() : slash;
    segments.emplace_back(pointer.substr(start, end - start));
    if (slash == std::string_view::npos) {
      break;
    }
    start = slash + 1;
  }
  return segments;
}

void visit_segments(
    nlohmann::json& value,
    const std::vector<std::string>& segments,
    std::size_t index,
    const std::string& pointer,
    const std::function<void(nlohmann::json&, const std::string&)>& visit) {
  if (index == segments.size()) {
    visit(value, pointer);
    return;
  }

  const auto& segment = segments[index];
  if (value.is_object()) {
    if (segment == kWildcard) {
      for (auto item = value.begin(); item != value.end(); ++item) {
        visit_segments(item.value(), segments, index + 1, pointer + "/" + item.key(),
                       visit);
      }
      return;
    }
    const auto found = value.find(segment);
    if (found != value.end()) {
      visit_segments(*found, segments, index + 1, pointer + "/" + segment, visit);
    }
    return;
  }

  if (value.is_array()) {
    if (segment == kWildcard) {
      for (std::size_t item = 0; item < value.size(); ++item) {
        visit_segments(value[item], segments, index + 1,
                       pointer + "/" + std::to_string(item), visit);
      }
      return;
    }
    try {
      const auto item = std::stoul(segment);
      if (item < value.size()) {
        visit_segments(value[item], segments, index + 1, pointer + "/" + segment, visit);
      }
    } catch (const std::exception&) {
      // Non-numeric segment on an array addresses nothing.
    }
  }
}

}  // namespace

void for_each_pointer_match(
    nlohmann::json& document,
    std::string_view pattern,
    const std::function<void(nlohmann::json& value, const std::string& pointer)>& visit) {
  visit_segments(document, split_pointer(pattern), 0, std::string{}, visit);
}

const nlohmann::json* find_pointer(const nlohmann::json& document, std::string_view pointer) {
  const nlohmann::json* current = &document;
  for (const auto& segment : split_pointer(pointer)) {
    if (current->is_object()) {
      const auto found = current->find(segment);
      if (found == current->end()) {
        return nullptr;
      }
      current = &*found;
      continue;
    }
    return nullptr;
  }
  return current;
}

}  // namespace svp::validation::equivalence
