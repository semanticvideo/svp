#pragma once

#include "export_plan.hpp"
#include "layer_result.hpp"
#include "output_transaction.hpp"
#include "package_reader.hpp"

#include "svp/blocks/block_stream.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace package_export {

struct DecodedBlock {
  svp::blocks::BlockHeaderV1 header;
  std::uint64_t ordinal = 0;
  std::uint64_t block_length = 0;
  std::uint64_t decoded_offset = 0;
};

// The decoded form of one SVPB block stream entry (Package_Export_v1.md
// Section 6), kept for resolving index records that point at its blocks.
struct DecodedBlockStream {
  std::string entry;
  std::string decoded_file;
  std::vector<DecodedBlock> blocks;  // stream order

  // The block whose header starts at `block_offset`, or nullptr.
  [[nodiscard]] const DecodedBlock* find_by_offset(
      std::uint64_t block_offset) const;
};

[[nodiscard]] std::string_view block_type_name(std::uint8_t block_type) noexcept;
[[nodiscard]] std::string_view dtype_name(std::uint32_t dtype) noexcept;

// Verifies every block of the stream (svp::blocks::parse_block_stream), then
// writes the block table and the concatenated decompressed payloads. Throws
// ExportError(invalid_block_stream) when any block fails verification.
[[nodiscard]] DecodedBlockStream export_block_stream(
    const PackageReader& reader, const PlannedLayer& layer,
    OutputTransaction& output, LayerResult& result);

}  // namespace package_export
