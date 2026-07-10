#include "transport_output_policy.hpp"

#include <algorithm>
#include <cctype>

namespace svp::builder::detail {

bool transport_output_path_matches_container(
    const std::filesystem::path& output_path,
    svp::package::IsoBmffContainerKind kind,
    std::string& error_message) {
  const char* required_extension = nullptr;
  switch (kind) {
    case svp::package::IsoBmffContainerKind::mp4:
      required_extension = ".mp4";
      break;
    case svp::package::IsoBmffContainerKind::quicktime:
      required_extension = ".mov";
      break;
    case svp::package::IsoBmffContainerKind::m4v:
      required_extension = ".m4v";
      break;
    case svp::package::IsoBmffContainerKind::m4a:
      required_extension = ".m4a";
      break;
    case svp::package::IsoBmffContainerKind::unknown:
    case svp::package::IsoBmffContainerKind::unsupported_derivative:
      error_message = "Cannot select an output suffix for this container family.";
      return false;
  }

  auto actual_extension = output_path.extension().string();
  std::ranges::transform(
      actual_extension, actual_extension.begin(),
      [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
  if (actual_extension != required_extension) {
    error_message = "Output path must use the " +
                    std::string(required_extension) +
                    " suffix for the detected container family.";
    return false;
  }
  return true;
}

}  // namespace svp::builder::detail
