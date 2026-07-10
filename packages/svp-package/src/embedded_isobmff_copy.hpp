#pragma once

#include "isobmff_top_level.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace svp::package::detail {

[[nodiscard]] bool copy_iso_bmff_with_embedding_change(
    const std::filesystem::path& input_path,
    const TopLevelScan& scan,
    const std::vector<EmbeddedSvpiInfo>& embeddings,
    std::ofstream& output,
    std::uint64_t insertion_offset,
    const std::filesystem::path* svpi_path,
    std::uint64_t payload_size,
    const std::array<std::uint8_t, 32>& payload_hash);

[[nodiscard]] bool copy_iso_bmff_with_embedding_change(
    const std::filesystem::path& input_path,
    const TopLevelScan& scan,
    const std::vector<EmbeddedSvpiInfo>& embeddings,
    std::ofstream& output,
    std::uint64_t insertion_offset,
    const std::filesystem::path* svpi_path,
    std::uint64_t payload_size,
    const std::array<std::uint8_t, 32>& payload_hash,
    std::uint64_t compact_box_size_limit);

}  // namespace svp::package::detail
