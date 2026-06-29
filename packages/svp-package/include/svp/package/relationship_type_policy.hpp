#pragma once

#include <string_view>

namespace svp::package {

enum class RelationshipClass {
  Support,
  Semantic,
  Unknown,
};

[[nodiscard]] RelationshipClass classify_relationship_type(std::string_view type);

[[nodiscard]] constexpr std::string_view relationship_class_to_string(RelationshipClass cls) {
  switch (cls) {
    case RelationshipClass::Support:  return "support";
    case RelationshipClass::Semantic: return "semantic";
    case RelationshipClass::Unknown:  return "unknown";
  }
  return "unknown";
}

}  // namespace svp::package
