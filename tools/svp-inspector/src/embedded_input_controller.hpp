#pragma once

#include "svp/package/embedded_package_session.hpp"
#include "svp/package/embedded_svpi.hpp"

#include <filesystem>
#include <memory>

namespace embedded_input_controller {

struct Decision {
  bool handled_as_transport = false;
  bool semantic_access_allowed = true;
  std::unique_ptr<svp::package::VerifiedEmbeddedPackageSession> session;

  [[nodiscard]] const svp::package::EmbeddedSvpiInspection& inspection()
      const noexcept;
};

[[nodiscard]] Decision preflight(const std::filesystem::path& path);

}  // namespace embedded_input_controller
