#include "equivalence/block_compare.hpp"

#include "equivalence/block_payload_rules.hpp"

#include "svp/blocks/block_payload.hpp"

#include <cstring>
#include <optional>
#include <utility>
#include <vector>

namespace svp::validation::equivalence {
namespace {

constexpr std::string_view kLayer = "block_stream";

svp::blocks::ParseResult parse_stream(const std::string& bytes,
                                      svp::blocks::BlockType block_type) {
  svp::blocks::ParseOptions options;
  options.required_block_type = block_type;
  options.allow_empty = true;
  std::size_t position = 0;
  return svp::blocks::parse_block_stream(
      bytes.size(), options,
      [&](std::byte* output, std::size_t byte_count, std::string& error_message) {
        if (byte_count > bytes.size() - position) {
          error_message = "block stream ended before the declared block range";
          return false;
        }
        std::memcpy(output, bytes.data() + position, byte_count);
        position += byte_count;
        return true;
      });
}

// Header fields that define block structure. Hashes are excluded: the
// payload hash is decided by the decoded payload rule, and the header hash
// covers the payload hash.
std::optional<std::string> first_structural_difference(const svp::blocks::BlockHeaderV1& left,
                                                       const svp::blocks::BlockHeaderV1& right) {
  const std::pair<std::string_view, bool> fields[] = {
      {"block_offset", left.offset == right.offset},
      {"version", left.version == right.version},
      {"header_size", left.header_size == right.header_size},
      {"block_type", left.block_type == right.block_type},
      {"compression", left.compression == right.compression},
      {"endian", left.endian == right.endian},
      {"flags", left.flags == right.flags},
      {"uncompressed_size", left.uncompressed_size == right.uncompressed_size},
      {"compressed_size", left.compressed_size == right.compressed_size},
      {"extent_0", left.extent_0 == right.extent_0},
      {"extent_1", left.extent_1 == right.extent_1},
      {"extent_2", left.extent_2 == right.extent_2},
      {"dtype", left.dtype == right.dtype},
      {"start_frame", left.start_frame == right.start_frame},
      {"frame_count", left.frame_count == right.frame_count},
      {"start_us", left.start_us == right.start_us},
      {"end_us", left.end_us == right.end_us},
  };
  for (const auto& [name, equal] : fields) {
    if (!equal) {
      return std::string{name};
    }
  }
  return std::nullopt;
}

void add_stream_finding(std::string_view entry,
                        std::string rule,
                        std::string location,
                        std::string detail,
                        EquivalenceLedger& ledger) {
  ledger.add(EquivalenceFinding{
      .outcome = EquivalenceOutcome::not_equivalent,
      .entry = std::string{entry},
      .layer = std::string{kLayer},
      .rule = std::move(rule),
      .location = std::move(location),
      .detail = std::move(detail),
  });
}

std::span<const std::byte> payload_of(const std::string& bytes,
                                      const svp::blocks::BlockHeaderV1& header) {
  return {reinterpret_cast<const std::byte*>(bytes.data()) + header.offset +
              header.header_size,
          static_cast<std::size_t>(header.compressed_size)};
}

}  // namespace

void compare_block_stream_entry(std::string_view entry,
                                svp::blocks::BlockType block_type,
                                const std::string& left_bytes,
                                const std::string& right_bytes,
                                EquivalenceLedger& ledger) {
  const auto left = parse_stream(left_bytes, block_type);
  const auto right = parse_stream(right_bytes, block_type);
  for (const auto* parsed : {&left, &right}) {
    if (!parsed->issues.empty()) {
      const auto& issue = parsed->issues.front();
      add_stream_finding(entry, "block_stream_readable",
                         "@" + std::to_string(issue.offset),
                         std::string{parsed == &left ? "left" : "right"} +
                             " block stream is not decodable: " + issue.message,
                         ledger);
      return;
    }
  }
  if (left.blocks.size() != right.blocks.size()) {
    add_stream_finding(entry, "block_count", "",
                       "left has " + std::to_string(left.blocks.size()) +
                           " blocks, right has " + std::to_string(right.blocks.size()),
                       ledger);
    return;
  }

  for (std::size_t index = 0; index < left.blocks.size(); ++index) {
    const auto& left_header = left.blocks[index];
    const auto& right_header = right.blocks[index];
    const BlockPairContext context{
        .entry = std::string{entry},
        .location = "block " + std::to_string(index) + " @" +
                    std::to_string(left_header.offset),
        .source_key = block_source_key(std::string{entry}, left_header.offset),
    };
    ledger.mark_compared(context.source_key);

    if (const auto field = first_structural_difference(left_header, right_header)) {
      ledger.mark_outcome(context.source_key, EquivalenceOutcome::not_equivalent);
      add_stream_finding(entry, "block_structure." + *field, context.location,
                         "block header field differs; block structure is exact-governed",
                         ledger);
      continue;
    }
    if (left_header.payload_blake3 == right_header.payload_blake3) {
      continue;
    }

    try {
      const auto left_payload = payload_of(left_bytes, left_header);
      const auto right_payload = payload_of(right_bytes, right_header);
      const auto left_decoded = svp::blocks::decompress_block_payload(
          left_payload.data(), left_payload.size(), left_header.uncompressed_size);
      const auto right_decoded = svp::blocks::decompress_block_payload(
          right_payload.data(), right_payload.size(), right_header.uncompressed_size);
      if (left_decoded == right_decoded) {
        ledger.add(EquivalenceFinding{
            .outcome = EquivalenceOutcome::canonicalization_only,
            .entry = std::string{entry},
            .layer = std::string{kLayer},
            .rule = "block_payload_decoded_identical",
            .location = context.location,
            .detail = "Compressed payload bytes differ; decoded payload is identical.",
        });
        continue;
      }
      compare_decoded_block_payloads(context, left_header, left_decoded, right_decoded,
                                     ledger);
    } catch (const std::exception& error) {
      ledger.mark_outcome(context.source_key, EquivalenceOutcome::not_equivalent);
      add_stream_finding(entry, "block_payload_decodable", context.location, error.what(),
                         ledger);
    }
  }
}

}  // namespace svp::validation::equivalence
