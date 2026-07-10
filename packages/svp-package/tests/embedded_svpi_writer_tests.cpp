#include "embedded_svpi_test_declarations.hpp"
#include "embedded_svpi_test_support.hpp"

#include "embedded_file_io.hpp"
#include "embedded_isobmff_copy.hpp"
#include "isobmff_top_level.hpp"
#include "svp/package/embedded_svpi_transport_profile.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

using namespace svp::package;
using namespace svp::package::test;

void test_extract_and_strip_are_byte_exact() {
  TempDirectory root;
  const auto original = root.path / "original.mp4";
  const auto svpi = root.path / "original.svpi";
  const auto embedded = root.path / "embedded.mp4";
  const auto extracted = root.path / "extracted.svpi";
  const auto stripped = root.path / "stripped.mp4";
  write_test_iso_bmff(original, true);
  write_bytes(svpi, {'P', 'K', 3, 4, 0, 1, 2, 3, 4, 5});

  CHECK_EMBEDDED(embed_svpi_in_iso_bmff(original, svpi, embedded).success);
  CHECK_EMBEDDED(extract_embedded_svpi(embedded, extracted).success);
  CHECK_EMBEDDED(strip_embedded_svpi(embedded, stripped).success);
  CHECK_EMBEDDED(read_bytes(extracted) == read_bytes(svpi));
  CHECK_EMBEDDED(read_bytes(stripped) == read_bytes(original));
}

void test_supported_container_families_embed_extract_and_strip_exactly() {
  TempDirectory root;
  const auto svpi = root.path / "package.svpi";
  write_bytes(svpi, {'P', 'K', 3, 4, 8, 7, 6, 5});

  struct Candidate {
    const char* extension;
    const char* brand;
  };
  for (const auto& candidate : {
           Candidate{"mov", "qt  "}, Candidate{"m4v", "M4V "},
           Candidate{"m4a", "M4A "}, Candidate{"mp4", "mp42"}}) {
    const auto original = root.path / ("original." + std::string(candidate.extension));
    const auto embedded = root.path / ("embedded." + std::string(candidate.extension));
    const auto stripped = root.path / ("stripped." + std::string(candidate.extension));
    const auto extracted = root.path /
        ("extracted-" + std::string(candidate.extension) + ".svpi");
    write_test_iso_bmff(
        original, true, false, false, false, candidate.brand);

    CHECK_EMBEDDED(embed_svpi_in_iso_bmff(original, svpi, embedded).success);
    CHECK_EMBEDDED(extract_embedded_svpi(embedded, extracted).success);
    CHECK_EMBEDDED(strip_embedded_svpi(embedded, stripped).success);
    CHECK_EMBEDDED(read_bytes(extracted) == read_bytes(svpi));
    CHECK_EMBEDDED(read_bytes(stripped) == read_bytes(original));
  }
}

void test_terminal_mfra_is_preserved_at_end() {
  TempDirectory root;
  const auto original = root.path / "fragmented.mp4";
  const auto svpi = root.path / "package.svpi";
  const auto replacement_svpi = root.path / "replacement.svpi";
  const auto embedded = root.path / "embedded.mp4";
  const auto replaced = root.path / "replaced.mp4";
  const auto stripped = root.path / "stripped.mp4";
  write_test_iso_bmff(original, true, true);
  write_bytes(svpi, {1, 2, 3});
  write_bytes(replacement_svpi, {4, 5, 6, 7});
  const auto original_size = std::filesystem::file_size(original);
  const auto result = embed_svpi_in_iso_bmff(original, svpi, embedded);
  CHECK_EMBEDDED(result.success);
  CHECK_EMBEDDED(result.inspection.embeddings.front().box_offset < original_size);
  EmbeddedSvpiWriteOptions replacement_options;
  replacement_options.replace_existing = true;
  CHECK_EMBEDDED(embed_svpi_in_iso_bmff(
      embedded, replacement_svpi, replaced, replacement_options).success);
  const auto original_bytes = read_bytes(original);
  const auto replaced_bytes = read_bytes(replaced);
  CHECK_EMBEDDED(std::equal(original_bytes.end() - 12,
                            original_bytes.end(), replaced_bytes.end() - 12));
  CHECK_EMBEDDED(strip_embedded_svpi(embedded, stripped).success);
  CHECK_EMBEDDED(read_bytes(stripped) == original_bytes);
}

void test_zero_sized_top_level_box_is_rejected_without_output() {
  TempDirectory root;
  const auto input = root.path / "zero.mp4";
  const auto svpi = root.path / "package.svpi";
  const auto output = root.path / "output.mp4";
  write_test_iso_bmff(input, true, false, true);
  write_bytes(svpi, {1, 2, 3});
  const auto result = embed_svpi_in_iso_bmff(input, svpi, output);
  CHECK_EMBEDDED(!result.success);
  CHECK_EMBEDDED(has_issue(result.inspection,
                           EmbeddedSvpiIssueCode::zero_sized_top_level_box));
  CHECK_EMBEDDED(!std::filesystem::exists(output));
}

void test_existing_embedding_requires_explicit_replacement() {
  TempDirectory root;
  const auto original = root.path / "original.mp4";
  const auto first = root.path / "first.svpi";
  const auto second = root.path / "second.svpi";
  const auto embedded = root.path / "embedded.mp4";
  const auto replaced = root.path / "replaced.mp4";
  const auto extracted = root.path / "extracted.svpi";
  write_test_iso_bmff(original, false);
  write_bytes(first, {1, 1, 1});
  write_bytes(second, {2, 2, 2, 2});
  CHECK_EMBEDDED(embed_svpi_in_iso_bmff(original, first, embedded).success);
  CHECK_EMBEDDED(!embed_svpi_in_iso_bmff(embedded, second, replaced).success);
  CHECK_EMBEDDED(!std::filesystem::exists(replaced));

  EmbeddedSvpiWriteOptions options;
  options.replace_existing = true;
  CHECK_EMBEDDED(embed_svpi_in_iso_bmff(embedded, second, replaced, options).success);
  CHECK_EMBEDDED(inspect_embedded_svpi(replaced, true).embeddings.size() == 1);
  CHECK_EMBEDDED(extract_embedded_svpi(replaced, extracted).success);
  CHECK_EMBEDDED(read_bytes(extracted) == read_bytes(second));
}

void test_ambiguous_fragment_tail_layouts_are_rejected() {
  TempDirectory root;
  const auto svpi = root.path / "package.svpi";
  write_bytes(svpi, {1, 2, 3});

  const auto mfro = root.path / "mfro.mp4";
  write_test_iso_bmff(mfro, true);
  auto bytes = read_bytes(mfro);
  bytes.insert(bytes.end(), {0, 0, 0, 8, 'm', 'f', 'r', 'o'});
  write_bytes(mfro, bytes);
  auto result = embed_svpi_in_iso_bmff(mfro, svpi, root.path / "mfro-out.mp4");
  CHECK_EMBEDDED(!result.success);
  CHECK_EMBEDDED(has_issue(result.inspection,
                           EmbeddedSvpiIssueCode::unsafe_tail_layout));

  const auto nonterminal_mfra = root.path / "nonterminal-mfra.mp4";
  write_test_iso_bmff(nonterminal_mfra, true, true);
  bytes = read_bytes(nonterminal_mfra);
  bytes.insert(bytes.end(), {0, 0, 0, 8, 'm', 'o', 'o', 'v'});
  write_bytes(nonterminal_mfra, bytes);
  result = embed_svpi_in_iso_bmff(
      nonterminal_mfra, svpi, root.path / "nonterminal-out.mp4");
  CHECK_EMBEDDED(!result.success);
  CHECK_EMBEDDED(has_issue(result.inspection,
                           EmbeddedSvpiIssueCode::unsafe_tail_layout));
}

void test_failures_leave_no_partial_output() {
  TempDirectory root;
  const auto invalid = root.path / "invalid.mp4";
  const auto svpi = root.path / "package.svpi";
  const auto output = root.path / "output.mp4";
  write_bytes(invalid, {0, 1, 2});
  write_bytes(svpi, {1, 2, 3});
  CHECK_EMBEDDED(!embed_svpi_in_iso_bmff(invalid, svpi, output).success);
  CHECK_EMBEDDED(!std::filesystem::exists(output));

  for (const auto& entry : std::filesystem::directory_iterator(root.path)) {
    CHECK_EMBEDDED(entry.path().filename().string().find(".svp-tmp-") ==
                   std::string::npos);
  }
}

void test_extended_uuid_box_full_lifecycle_with_synthetic_limit() {
  TempDirectory root;
  const auto original = root.path / "original.mp4";
  const auto svpi = root.path / "large-for-synthetic-limit.svpi";
  const auto embedded = root.path / "embedded.mp4";
  const auto extracted = root.path / "extracted.svpi";
  const auto stripped = root.path / "stripped.mp4";
  write_test_iso_bmff(original, true);
  std::vector<std::uint8_t> payload(257);
  for (std::size_t index = 0; index < payload.size(); ++index) {
    payload[index] = static_cast<std::uint8_t>(index % 251);
  }
  write_bytes(svpi, payload);

  const auto scan = svp::package::detail::scan_top_level_boxes(original);
  CHECK_EMBEDDED(scan.valid);
  std::uint64_t payload_size = 0;
  std::array<std::uint8_t, 32> payload_hash{};
  CHECK_EMBEDDED(svp::package::detail::hash_file(
      svpi, payload_size, payload_hash));
  constexpr std::uint64_t synthetic_compact_box_limit =
      24 + svp::package::kEmbeddedSvpiEnvelopeSize + 256;
  CHECK_EMBEDDED(payload_size + 24 +
                     svp::package::kEmbeddedSvpiEnvelopeSize >
                 synthetic_compact_box_limit);

  std::ofstream output(embedded, std::ios::binary | std::ios::trunc);
  CHECK_EMBEDDED(svp::package::detail::copy_iso_bmff_with_embedding_change(
      original, scan, {}, output, scan.file_size, &svpi, payload_size,
      payload_hash, synthetic_compact_box_limit));
  output.close();
  CHECK_EMBEDDED(output.good());

  const auto inspection = inspect_embedded_svpi(embedded, true);
  CHECK_EMBEDDED(inspection.has_single_valid_embedding());
  CHECK_EMBEDDED(inspection.embeddings.front().hash_verified);
  CHECK_EMBEDDED(inspection.embeddings.front().hash_matches);
  const auto embedded_bytes = read_bytes(embedded);
  const auto box_offset = static_cast<std::size_t>(
      inspection.embeddings.front().box_offset);
  CHECK_EMBEDDED(svp::package::detail::read_be32(
      embedded_bytes.data() + box_offset) == 1);
  CHECK_EMBEDDED(svp::package::detail::read_be64(
      embedded_bytes.data() + box_offset + 8) ==
      inspection.embeddings.front().box_size);

  CHECK_EMBEDDED(extract_embedded_svpi(embedded, extracted).success);
  CHECK_EMBEDDED(strip_embedded_svpi(embedded, stripped).success);
  CHECK_EMBEDDED(read_bytes(extracted) == payload);
  CHECK_EMBEDDED(read_bytes(stripped) == read_bytes(original));
}

}  // namespace

void run_embedded_svpi_writer_tests() {
  test_extract_and_strip_are_byte_exact();
  test_supported_container_families_embed_extract_and_strip_exactly();
  test_terminal_mfra_is_preserved_at_end();
  test_zero_sized_top_level_box_is_rejected_without_output();
  test_existing_embedding_requires_explicit_replacement();
  test_ambiguous_fragment_tail_layouts_are_rejected();
  test_failures_leave_no_partial_output();
  test_extended_uuid_box_full_lifecycle_with_synthetic_limit();
}
