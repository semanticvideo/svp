#pragma once

#include "equivalence/equivalence_ledger.hpp"

#include "svp/blocks/block_stream.hpp"

#include <cstddef>
#include <span>
#include <string>

namespace svp::validation::equivalence {

// Identifies one block pair for findings.
struct BlockPairContext {
  std::string entry;
  std::string location;   // "block <n> @<offset>"
  std::string source_key; // block_source_key(entry, offset)
};

// Applies the Section 5.16.2 rule for the block type to two decoded
// payloads that differ. The headers have already matched exactly.
void compare_decoded_block_payloads(const BlockPairContext& context,
                                    const svp::blocks::BlockHeaderV1& header,
                                    std::span<const std::byte> left,
                                    std::span<const std::byte> right,
                                    EquivalenceLedger& ledger);

}  // namespace svp::validation::equivalence
