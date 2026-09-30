#include "equivalence/derived_digest_registry.hpp"

#include <array>

namespace svp::validation::equivalence {
namespace {

constexpr std::array<std::string_view, 1> kIndexSqliteSource{"index/index.sqlite"};
constexpr std::array<std::string_view, 1> kManifestSource{"manifest.json"};
constexpr std::array<std::string_view, 1> kEmbeddingSetsSource{
    "embeddings/embedding_sets.json"};
// binary_blocks_manifest.json is a builder staging file that is not packaged;
// it is derived from the three block streams and their index records.
constexpr std::array<std::string_view, 6> kBlockStreamSources{
    "spatial/depth.blocks.svpdz",       "spatial/depth.index.jsonl",
    "spatial/masks.blocks.svpmz",       "spatial/masks.index.jsonl",
    "embeddings/embeddings.blocks.svpez", "embeddings/embeddings.index.jsonl",
};

constexpr std::string_view kBlockHashReason =
    "SVPB block hash; decided by the decoded payload's source-layer rule.";

constexpr std::array kJsonDerivedDigestFields{
    JsonDerivedDigestField{
        .entry = "index/index_manifest.json",
        .pointer = "/sqlite_file_blake3",
        .sources = kIndexSqliteSource,
        .reason = "Hash of raw index.sqlite bytes; Section 17.5 compares the "
                  "logical row stream, never raw SQLite bytes.",
    },
    JsonDerivedDigestField{
        .entry = "index/index_manifest.json",
        .pointer = "/logical_rows_blake3",
        .sources = kIndexSqliteSource,
        .reason = "Hash of the canonical logical row stream; Section 17.5 "
                  "cross-package equivalence is source-layer aware.",
    },
    JsonDerivedDigestField{
        .entry = "index/index_manifest.json",
        .pointer = "/created_from/manifest_blake3",
        .sources = kManifestSource,
        .reason = "Hash of manifest.json bytes.",
    },
    JsonDerivedDigestField{
        .entry = "index/index_manifest.json",
        .pointer = "/created_from/embedding_sets_blake3",
        .sources = kEmbeddingSetsSource,
        .reason = "Hash of embeddings/embedding_sets.json bytes.",
    },
    JsonDerivedDigestField{
        .entry = "index/index_manifest.json",
        .pointer = "/created_from/binary_blocks_manifest_blake3",
        .sources = kBlockStreamSources,
        .reason = "Hash of the builder's block manifest, derived from the "
                  "packaged block streams and block index records.",
    },
    JsonDerivedDigestField{.entry = "spatial/depth.index.jsonl",
                           .pointer = "/payload_blake3",
                           .kind = DigestSourceKind::record_block,
                           .reason = kBlockHashReason},
    JsonDerivedDigestField{.entry = "spatial/depth.index.jsonl",
                           .pointer = "/block_blake3",
                           .kind = DigestSourceKind::record_block,
                           .reason = kBlockHashReason},
    JsonDerivedDigestField{.entry = "spatial/masks.index.jsonl",
                           .pointer = "/payload_blake3",
                           .kind = DigestSourceKind::record_block,
                           .reason = kBlockHashReason},
    JsonDerivedDigestField{.entry = "spatial/masks.index.jsonl",
                           .pointer = "/block_blake3",
                           .kind = DigestSourceKind::record_block,
                           .reason = kBlockHashReason},
    JsonDerivedDigestField{.entry = "embeddings/embeddings.index.jsonl",
                           .pointer = "/payload_blake3",
                           .kind = DigestSourceKind::record_block,
                           .reason = kBlockHashReason},
    JsonDerivedDigestField{.entry = "embeddings/embeddings.index.jsonl",
                           .pointer = "/block_blake3",
                           .kind = DigestSourceKind::record_block,
                           .reason = kBlockHashReason},
};

constexpr std::array kSqliteDerivedDigestColumns{
    SqliteDerivedDigestColumn{.table = "binary_blocks",
                              .column = "payload_blake3",
                              .block_file_column = "block_file",
                              .block_offset_column = "block_offset",
                              .reason = kBlockHashReason},
    SqliteDerivedDigestColumn{.table = "binary_blocks",
                              .column = "header_blake3",
                              .block_file_column = "block_file",
                              .block_offset_column = "block_offset",
                              .reason = kBlockHashReason},
    SqliteDerivedDigestColumn{.table = "binary_blocks",
                              .column = "block_blake3",
                              .block_file_column = "block_file",
                              .block_offset_column = "block_offset",
                              .reason = kBlockHashReason},
};

}  // namespace

std::span<const JsonDerivedDigestField> json_derived_digest_fields() {
  return kJsonDerivedDigestFields;
}

std::span<const SqliteDerivedDigestColumn> sqlite_derived_digest_columns() {
  return kSqliteDerivedDigestColumns;
}

}  // namespace svp::validation::equivalence
