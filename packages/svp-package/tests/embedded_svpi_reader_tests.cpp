#include "embedded_svpi_test_declarations.hpp"
#include "embedded_svpi_test_support.hpp"

#include "embedded_svpi_box.hpp"
#include "isobmff_top_level.hpp"
#include "svp/package/embedded_svpi_transport_profile.hpp"
#include "svp/package/package_layout.hpp"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <stdexcept>

namespace {

using namespace svp::package;
using namespace svp::package::test;

struct EmbeddedFixture {
  TempDirectory root;
  std::filesystem::path original = root.path / "original.mp4";
  std::filesystem::path svpi = root.path / "package.svpi";
  std::filesystem::path embedded = root.path / "embedded.mp4";

  EmbeddedFixture() {
    write_test_iso_bmff(original, true);
    write_bytes(svpi, {'P', 'K', 3, 4, 's', 'v', 'p', 'i'});
    CHECK_EMBEDDED(embed_svpi_in_iso_bmff(original, svpi, embedded).success);
  }
};

void test_normal_container_and_unrelated_uuid_are_not_embeddings() {
  TempDirectory root;
  const auto normal = root.path / "normal.mp4";
  write_test_iso_bmff(normal, true, false, false, true);
  const auto inspection = inspect_embedded_svpi(normal);
  CHECK_EMBEDDED(inspection.container_structure_valid);
  CHECK_EMBEDDED(inspection.embeddings.empty());
  CHECK_EMBEDDED(has_issue(inspection, EmbeddedSvpiIssueCode::no_embedding));
}

void test_fast_start_and_moov_after_mdat_are_supported() {
  TempDirectory root;
  for (const bool fast_start : {false, true}) {
    const auto input = root.path / (fast_start ? "fast.mp4" : "tail-moov.mp4");
    const auto output = root.path / (fast_start ? "fast-out.mp4" : "tail-out.mp4");
    const auto svpi = root.path / "package.svpi";
    write_test_iso_bmff(input, fast_start);
    write_bytes(svpi, {1, 2, 3, 4});
    const auto result = embed_svpi_in_iso_bmff(input, svpi, output);
    CHECK_EMBEDDED(result.success);
    CHECK_EMBEDDED(result.inspection.has_single_valid_embedding());
  }
}

void test_supported_brand_families_are_detected_without_extension_gates() {
  TempDirectory root;
  struct Candidate {
    const char* filename;
    const char* brand;
    IsoBmffContainerKind expected_kind;
  };
  const Candidate candidates[] = {
      {"quicktime.bin", "qt  ", IsoBmffContainerKind::quicktime},
      {"video.data", "M4V ", IsoBmffContainerKind::m4v},
      {"audio.payload", "M4A ", IsoBmffContainerKind::m4a},
      {"movie.unknown", "mp42", IsoBmffContainerKind::mp4},
  };

  for (const auto& candidate : candidates) {
    const auto path = root.path / candidate.filename;
    write_test_iso_bmff(path, true, false, false, false, candidate.brand);
    const auto container = inspect_iso_bmff_container(path);
    CHECK_EMBEDDED(container.signature_present);
    CHECK_EMBEDDED(container.structure_valid);
    CHECK_EMBEDDED(container.supported);
    CHECK_EMBEDDED(container.kind == candidate.expected_kind);
    CHECK_EMBEDDED(container.major_brand == candidate.brand);
  }
}

void test_unsupported_brand_and_non_bmff_inputs_are_distinguished() {
  TempDirectory root;
  const auto unsupported = root.path / "image.avif";
  write_test_iso_bmff(unsupported, true, false, false, false, "avif");
  auto inspection = inspect_embedded_svpi(unsupported);
  CHECK_EMBEDDED(inspection.container.signature_present);
  CHECK_EMBEDDED(inspection.container.structure_valid);
  CHECK_EMBEDDED(!inspection.container.supported);
  CHECK_EMBEDDED(has_issue(
      inspection, EmbeddedSvpiIssueCode::unsupported_container));

  const auto unrelated = root.path / "not-media.mov";
  write_bytes(unrelated, {'P', 'K', 3, 4, 1, 2, 3, 4});
  inspection = inspect_embedded_svpi(unrelated);
  CHECK_EMBEDDED(!inspection.container.signature_present);
  CHECK_EMBEDDED(!inspection.container_structure_valid);
  CHECK_EMBEDDED(has_issue(
      inspection, EmbeddedSvpiIssueCode::unsupported_container));
}

void test_ftyp_brand_table_is_bounded() {
  TempDirectory root;
  const auto path = root.path / "brand-bomb.mov";
  constexpr std::size_t brand_count = 65;
  constexpr std::size_t box_size = 8 + 8 + brand_count * 4;
  std::vector<std::uint8_t> bytes(box_size, 0);
  bytes[2] = 1;
  bytes[3] = static_cast<std::uint8_t>(box_size - 256);
  std::copy_n("ftyp", 4, bytes.begin() + 4);
  std::copy_n("qt  ", 4, bytes.begin() + 8);
  for (std::size_t index = 0; index < brand_count; ++index) {
    std::copy_n("qt  ", 4, bytes.begin() + 16 + index * 4);
  }
  write_bytes(path, bytes);
  const auto container = inspect_iso_bmff_container(path);
  CHECK_EMBEDDED(container.signature_present);
  CHECK_EMBEDDED(!container.structure_valid);
  CHECK_EMBEDDED(!container.supported);
}

void test_scanner_seeks_over_sparse_mdat() {
  TempDirectory root;
  const auto path = root.path / "sparse.mp4";
  constexpr std::uint64_t mdat_size = 128ULL * 1024ULL * 1024ULL;
  write_sparse_iso_bmff(path, mdat_size);
  const auto inspection = inspect_embedded_svpi(path);
  CHECK_EMBEDDED(inspection.container_structure_valid);
  CHECK_EMBEDDED(inspection.scanner_bytes_read < 128);
  CHECK_EMBEDDED(inspection.file_size > mdat_size);
}

void test_truncated_and_invalid_box_headers_are_rejected() {
  TempDirectory root;
  const auto truncated = root.path / "truncated.mp4";
  write_bytes(truncated, {0, 0, 0, 16, 'f', 't', 'y', 'p'});
  auto inspection = inspect_embedded_svpi(truncated);
  CHECK_EMBEDDED(!inspection.container_structure_valid);
  CHECK_EMBEDDED(has_issue(inspection, EmbeddedSvpiIssueCode::invalid_box_structure));

  const auto undersized = root.path / "undersized.mp4";
  write_bytes(undersized, {0, 0, 0, 4, 'f', 't', 'y', 'p'});
  inspection = inspect_embedded_svpi(undersized);
  CHECK_EMBEDDED(!inspection.container_structure_valid);

  const auto oversized = root.path / "oversized.mp4";
  write_bytes(oversized, {0, 0, 0, 1, 'm', 'd', 'a', 't',
                          0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff});
  inspection = inspect_embedded_svpi(oversized);
  CHECK_EMBEDDED(!inspection.container_structure_valid);
}

void test_envelope_version_length_and_hash_errors() {
  EmbeddedFixture fixture;
  const auto base = inspect_embedded_svpi(fixture.embedded);
  const auto envelope_offset = base.embeddings.front().box_offset + 24;

  const auto version = fixture.root.path / "version.mp4";
  std::filesystem::copy_file(fixture.embedded, version);
  overwrite_byte(version, envelope_offset + 9, 2);
  auto inspection = inspect_embedded_svpi(version, true);
  CHECK_EMBEDDED(has_issue(inspection, EmbeddedSvpiIssueCode::unsupported_profile_version));

  const auto magic = fixture.root.path / "magic.mp4";
  std::filesystem::copy_file(fixture.embedded, magic);
  overwrite_byte(magic, envelope_offset, 0);
  inspection = inspect_embedded_svpi(magic, true);
  CHECK_EMBEDDED(has_issue(inspection, EmbeddedSvpiIssueCode::invalid_envelope_magic));

  const auto flags = fixture.root.path / "flags.mp4";
  std::filesystem::copy_file(fixture.embedded, flags);
  overwrite_byte(flags, envelope_offset + 15, 1);
  inspection = inspect_embedded_svpi(flags, true);
  CHECK_EMBEDDED(has_issue(inspection, EmbeddedSvpiIssueCode::unsupported_flags));

  const auto reserved = fixture.root.path / "reserved.mp4";
  std::filesystem::copy_file(fixture.embedded, reserved);
  overwrite_byte(reserved, envelope_offset + 63, 1);
  inspection = inspect_embedded_svpi(reserved, true);
  CHECK_EMBEDDED(has_issue(inspection, EmbeddedSvpiIssueCode::unsupported_flags));

  const auto envelope_size = fixture.root.path / "envelope-size.mp4";
  std::filesystem::copy_file(fixture.embedded, envelope_size);
  overwrite_byte(envelope_size, envelope_offset + 11, 63);
  inspection = inspect_embedded_svpi(envelope_size, true);
  CHECK_EMBEDDED(has_issue(inspection, EmbeddedSvpiIssueCode::invalid_envelope_size));

  const auto payload_length = fixture.root.path / "payload-length.mp4";
  std::filesystem::copy_file(fixture.embedded, payload_length);
  overwrite_be64(payload_length, envelope_offset + 16, 9999);
  inspection = inspect_embedded_svpi(payload_length, true);
  CHECK_EMBEDDED(has_issue(inspection, EmbeddedSvpiIssueCode::payload_length_mismatch));

  const auto hash = fixture.root.path / "hash.mp4";
  std::filesystem::copy_file(fixture.embedded, hash);
  overwrite_byte(hash, base.embeddings.front().payload_offset, 0xff);
  inspection = inspect_embedded_svpi(hash, true);
  CHECK_EMBEDDED(has_issue(inspection, EmbeddedSvpiIssueCode::payload_hash_mismatch));
  const auto layout = read_package_layout(hash);
  CHECK_EMBEDDED(!layout.has_value());
  CHECK_EMBEDDED(layout.error_message().find("BLAKE3") != std::string::npos);

  const auto truncated = fixture.root.path / "truncated-envelope.mp4";
  std::filesystem::copy_file(fixture.embedded, truncated);
  constexpr std::uint32_t truncated_box_size = 24 + 20;
  overwrite_be32(truncated, base.embeddings.front().box_offset,
                 truncated_box_size);
  std::filesystem::resize_file(
      truncated, base.embeddings.front().box_offset + truncated_box_size);
  inspection = inspect_embedded_svpi(truncated);
  CHECK_EMBEDDED(inspection.container_structure_valid);
  CHECK_EMBEDDED(has_issue(inspection, EmbeddedSvpiIssueCode::truncated_uuid_box));
}

void test_wrong_uuid_and_duplicate_embeddings() {
  EmbeddedFixture fixture;
  const auto base = inspect_embedded_svpi(fixture.embedded);
  const auto wrong = fixture.root.path / "wrong-uuid.mp4";
  std::filesystem::copy_file(fixture.embedded, wrong);
  overwrite_byte(wrong, base.embeddings.front().box_offset + 8, 0);
  auto inspection = inspect_embedded_svpi(wrong);
  CHECK_EMBEDDED(inspection.embeddings.empty());

  auto bytes = read_bytes(fixture.embedded);
  const auto box_begin = bytes.begin() +
      static_cast<std::ptrdiff_t>(base.embeddings.front().box_offset);
  const std::vector<std::uint8_t> box(box_begin, bytes.end());
  bytes.insert(bytes.end(), box.begin(), box.end());
  const auto duplicate = fixture.root.path / "duplicate.mp4";
  write_bytes(duplicate, bytes);
  inspection = inspect_embedded_svpi(duplicate);
  CHECK_EMBEDDED(inspection.embeddings.size() == 2);
  CHECK_EMBEDDED(has_issue(inspection, EmbeddedSvpiIssueCode::duplicate_embeddings));
}

void test_compact_extended_and_overflow_header_selection() {
  const auto compact = svp::package::detail::make_svpi_uuid_box_header(1024);
  CHECK_EMBEDDED(compact.size() == 24);
  CHECK_EMBEDDED(svp::package::detail::read_be32(compact.data()) ==
                 24 + kEmbeddedSvpiEnvelopeSize + 1024);

  const auto threshold = kIsoBmffMaxCompactBoxSize - 24 - kEmbeddedSvpiEnvelopeSize;
  const auto extended = svp::package::detail::make_svpi_uuid_box_header(threshold + 1);
  CHECK_EMBEDDED(extended.size() == 32);
  CHECK_EMBEDDED(svp::package::detail::read_be32(extended.data()) == 1);
  CHECK_EMBEDDED(svp::package::detail::read_be64(extended.data() + 8) ==
                 32 + kEmbeddedSvpiEnvelopeSize + threshold + 1);

  bool threw = false;
  try {
    (void)svp::package::detail::make_svpi_uuid_box_header(
        std::numeric_limits<std::uint64_t>::max());
  } catch (const std::overflow_error&) {
    threw = true;
  }
  CHECK_EMBEDDED(threw);
}

}  // namespace

void run_embedded_svpi_reader_tests() {
  test_normal_container_and_unrelated_uuid_are_not_embeddings();
  test_fast_start_and_moov_after_mdat_are_supported();
  test_supported_brand_families_are_detected_without_extension_gates();
  test_unsupported_brand_and_non_bmff_inputs_are_distinguished();
  test_ftyp_brand_table_is_bounded();
  test_scanner_seeks_over_sparse_mdat();
  test_truncated_and_invalid_box_headers_are_rejected();
  test_envelope_version_length_and_hash_errors();
  test_wrong_uuid_and_duplicate_embeddings();
  test_compact_extended_and_overflow_header_selection();
}
