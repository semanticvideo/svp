#pragma once

// Honest placeholders the vision lane writes when a model stage did not run
// (see spatial_embedding_placeholders.hpp for the file contract).

#include <filesystem>
#include <nlohmann/json.hpp>

namespace svp::package::detail {

[[nodiscard]] nlohmann::json make_spatial_placeholder_processor();
[[nodiscard]] nlohmann::json make_embedding_placeholder_processor();

// spatial/depth.index.jsonl and a zero-length spatial/depth.blocks.svpdz.
void write_depth_placeholder_files(const std::filesystem::path& staging_dir);
// Empty spatial/masks.index.jsonl and a zero-length masks block stream.
void write_mask_placeholder_files(const std::filesystem::path& staging_dir);
// {"sets": []}, an empty embedding index, and a zero-length block stream.
void write_embedding_placeholder_files(const std::filesystem::path& staging_dir);

}  // namespace svp::package::detail
