#pragma once

#include "svp/package/iso_bmff_container.hpp"

#include <filesystem>
#include <string>

namespace svp::builder::detail {

[[nodiscard]] bool transport_output_path_matches_container(
    const std::filesystem::path& output_path,
    svp::package::IsoBmffContainerKind kind,
    std::string& error_message);

}  // namespace svp::builder::detail
