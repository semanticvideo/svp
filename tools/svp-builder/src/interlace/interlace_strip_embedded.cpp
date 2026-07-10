#include "svp/builder/embedded_interlace.hpp"

#include "svp/package/embedded_svpi.hpp"
#include "svp/core/path.hpp"

namespace svp::builder {

StripEmbeddedResult interlace_strip_embedded(
    const StripEmbeddedOptions& options) {
  StripEmbeddedResult result;
  result.output_path = options.output_path;
  if (!svp::core::has_extension(options.mp4_path, ".mp4") ||
      !svp::core::has_extension(options.output_path, ".mp4")) {
    result.error_message =
        "strip-embedded requires .mp4 input and output paths.";
    return result;
  }
  const auto stripped = svp::package::strip_embedded_svpi(
      options.mp4_path, options.output_path, options.overwrite_output);
  result.success = stripped.success;
  result.error_message = stripped.error_message;
  return result;
}

}  // namespace svp::builder
