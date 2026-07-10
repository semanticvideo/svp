#include "embedded_input_controller.hpp"

#include "svp/package/package_probe.hpp"

namespace embedded_input_controller {

const svp::package::EmbeddedSvpiInspection& Decision::inspection()
    const noexcept {
  return session->inspection();
}

Decision preflight(const std::filesystem::path& path) {
  Decision decision;
  const auto probe = svp::package::probe_package(path);
  decision.handled_as_transport =
      probe.iso_bmff.signature_present ||
      (!probe.has_svp_extension && !probe.has_svpi_extension);
  if (!decision.handled_as_transport) {
    return decision;
  }

  decision.session =
      std::make_unique<svp::package::VerifiedEmbeddedPackageSession>(path);
  decision.semantic_access_allowed = decision.session->valid();
  return decision;
}

}  // namespace embedded_input_controller
