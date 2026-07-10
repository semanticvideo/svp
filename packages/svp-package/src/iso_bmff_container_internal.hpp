#pragma once

#include "isobmff_top_level.hpp"
#include "svp/package/iso_bmff_container.hpp"

namespace svp::package::detail {

[[nodiscard]] IsoBmffContainerInfo classify_iso_bmff_container(
    const std::filesystem::path& path,
    const TopLevelScan& scan);

}  // namespace svp::package::detail
