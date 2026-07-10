#include "transport/transport_output_policy.hpp"

#include <stdexcept>
#include <string>

namespace {

void require(bool condition) {
  if (!condition) {
    throw std::runtime_error("transport output policy requirement failed");
  }
}

}  // namespace

int main() {
  using svp::package::IsoBmffContainerKind;
  std::string error;
  require(svp::builder::detail::transport_output_path_matches_container(
      "semantic.MOV", IsoBmffContainerKind::quicktime, error));
  require(svp::builder::detail::transport_output_path_matches_container(
      "semantic.mp4", IsoBmffContainerKind::mp4, error));
  require(svp::builder::detail::transport_output_path_matches_container(
      "semantic.m4v", IsoBmffContainerKind::m4v, error));
  require(svp::builder::detail::transport_output_path_matches_container(
      "semantic.m4a", IsoBmffContainerKind::m4a, error));
  require(!svp::builder::detail::transport_output_path_matches_container(
      "semantic.bin", IsoBmffContainerKind::m4a, error));
  require(error.find(".m4a") != std::string::npos);
  return 0;
}
