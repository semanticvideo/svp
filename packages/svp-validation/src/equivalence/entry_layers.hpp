#pragma once

#include "svp/blocks/block_stream.hpp"

#include <optional>
#include <string_view>

namespace svp::validation::equivalence {

// Which Section 5.16.2 layer governs a package entry.
enum class EntryLayer {
  directory,        // ZIP directory marker: presence only
  sqlite_index,     // Section 17.5 logical comparison
  block_stream,     // SVPB depth/mask/embedding stream
  canonical_json,   // canonicalized JSON
  canonical_jsonl,  // canonicalized JSONL
  exact_bytes,      // original media, lossless audio, and every other file
};

struct EntryLayerInfo {
  EntryLayer layer = EntryLayer::exact_bytes;
  std::string_view name;  // reported layer name
  std::optional<svp::blocks::BlockType> block_type;
};

[[nodiscard]] EntryLayerInfo classify_entry(std::string_view entry);

}  // namespace svp::validation::equivalence
