#include "svp/builder/embedded_svpi_transport.hpp"

#include "svp/package/embedded_svpi.hpp"

#include "transport_output_policy.hpp"

namespace svp::builder {

EmbeddedTransportStripResult strip_svpi_transport(
    const EmbeddedTransportStripOptions& options) {
  EmbeddedTransportStripResult result;
  result.output_path = options.output_path;
  const auto inspection = svp::package::inspect_embedded_svpi(
      options.container_path, false);
  if (inspection.container.supported &&
      !detail::transport_output_path_matches_container(
          options.output_path, inspection.container.kind,
          result.error_message)) {
    return result;
  }
  const auto stripped = svp::package::strip_embedded_svpi(
      options.container_path, options.output_path, options.overwrite_output);
  result.success = stripped.success;
  result.error_message = stripped.error_message;
  return result;
}

}  // namespace svp::builder
