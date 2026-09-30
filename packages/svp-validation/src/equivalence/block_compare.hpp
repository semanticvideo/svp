#pragma once

#include "equivalence/equivalence_ledger.hpp"

#include "svp/blocks/block_stream.hpp"

#include <string>
#include <string_view>

namespace svp::validation::equivalence {

// Compares two SVPB block streams (RC2 Sections 14.5 and 5.16.2). Block
// structure (count, offsets, header fields, compressed and uncompressed
// sizes) is exact. Payloads whose bytes differ are decoded and compared by
// the Default Equivalence Profile v1 rule of the block type: depth
// MAE/p99/Spearman, embedding cosine, mask IoU and boundary displacement.
// Every block is recorded in the ledger under block_source_key() so digest
// fields that reference it can be resolved.
void compare_block_stream_entry(std::string_view entry,
                                svp::blocks::BlockType block_type,
                                const std::string& left_bytes,
                                const std::string& right_bytes,
                                EquivalenceLedger& ledger);

}  // namespace svp::validation::equivalence
