#pragma once

#include "mp4_top_level.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace svp::package::detail {

[[nodiscard]] bool copy_mp4_with_embedding_change(
    const std::filesystem::path& input_path,
    const TopLevelScan& scan,
    const std::vector<EmbeddedSvpiInfo>& embeddings,
    std::ofstream& output,
    std::uint64_t insertion_offset,
    const std::filesystem::path* svpi_path,
    std::uint64_t payload_size,
    const std::array<std::uint8_t, 32>& payload_hash);

}  // namespace svp::package::detail
