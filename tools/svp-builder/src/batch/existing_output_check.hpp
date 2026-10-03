#pragma once

// build-batch --resume: is an output that already exists complete and made
// from this source? An .svp validates and holds this source, byte for byte,
// as its original media; an SVPI validates and its media binding verifies
// against the source; an embedded SVPI transport validates and binds the
// source (the checks interlace create-batch applies to existing outputs).

#include "svp/builder/video_build_parameters.hpp"

#include <filesystem>
#include <string>

namespace svp::builder::batch {

// False, with why in `reason`, when the output must be built again.
[[nodiscard]] bool existing_output_is_complete(VideoOutputFormat format,
                                               const std::filesystem::path& output,
                                               const std::filesystem::path& source,
                                               std::string& reason);

}  // namespace svp::builder::batch
