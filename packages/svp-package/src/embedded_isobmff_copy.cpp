#include "embedded_isobmff_copy.hpp"

#include "embedded_file_io.hpp"
#include "embedded_svpi_box.hpp"
#include "svp/package/embedded_svpi_transport_profile.hpp"

#include <algorithm>
#include <array>

namespace svp::package::detail {
namespace {

bool write_uuid_box(std::ofstream& output,
                    const std::filesystem::path& svpi_path,
                    std::uint64_t payload_size,
                    const std::array<std::uint8_t, 32>& payload_hash,
                    std::uint64_t compact_box_size_limit) {
  const auto box_header = make_svpi_uuid_box_header(
      payload_size, compact_box_size_limit);
  output.write(reinterpret_cast<const char*>(box_header.data()),
               static_cast<std::streamsize>(box_header.size()));

  std::array<std::uint8_t, kEmbeddedSvpiEnvelopeSize> envelope{};
  std::copy(kEmbeddedSvpiEnvelopeMagic.begin(), kEmbeddedSvpiEnvelopeMagic.end(),
            envelope.begin());
  write_be16(envelope.data() + 8, kEmbeddedSvpiProfileVersion);
  write_be16(envelope.data() + 10, kEmbeddedSvpiEnvelopeSize);
  write_be32(envelope.data() + 12, 0);
  write_be64(envelope.data() + 16, payload_size);
  std::copy(payload_hash.begin(), payload_hash.end(), envelope.begin() + 24);
  output.write(reinterpret_cast<const char*>(envelope.data()), envelope.size());
  if (!output) {
    return false;
  }
  std::ifstream svpi(svpi_path, std::ios::binary);
  return svpi && copy_bytes(svpi, output, 0, payload_size);
}

}  // namespace

bool copy_iso_bmff_with_embedding_change(
    const std::filesystem::path& input_path,
    const TopLevelScan& scan,
    const std::vector<EmbeddedSvpiInfo>& embeddings,
    std::ofstream& output,
    std::uint64_t insertion_offset,
    const std::filesystem::path* svpi_path,
    std::uint64_t payload_size,
    const std::array<std::uint8_t, 32>& payload_hash) {
  return copy_iso_bmff_with_embedding_change(
      input_path, scan, embeddings, output, insertion_offset, svpi_path,
      payload_size, payload_hash, kIsoBmffMaxCompactBoxSize);
}

bool copy_iso_bmff_with_embedding_change(
    const std::filesystem::path& input_path,
    const TopLevelScan& scan,
    const std::vector<EmbeddedSvpiInfo>& embeddings,
    std::ofstream& output,
    std::uint64_t insertion_offset,
    const std::filesystem::path* svpi_path,
    std::uint64_t payload_size,
    const std::array<std::uint8_t, 32>& payload_hash,
    std::uint64_t compact_box_size_limit) {
  std::ifstream input(input_path, std::ios::binary);
  if (!input) {
    return false;
  }
  bool inserted = false;
  for (const auto& box : scan.boxes) {
    if (!inserted && box.offset == insertion_offset && svpi_path != nullptr) {
      if (!write_uuid_box(output, *svpi_path, payload_size, payload_hash,
                          compact_box_size_limit)) {
        return false;
      }
      inserted = true;
    }
    const bool remove = std::ranges::any_of(
        embeddings, [&](const EmbeddedSvpiInfo& embedding) {
          return embedding.box_offset == box.offset && embedding.box_size == box.size;
        });
    if (!remove && !copy_bytes(input, output, box.offset, box.size)) {
      return false;
    }
  }
  if (!inserted && insertion_offset == scan.file_size && svpi_path != nullptr) {
    return write_uuid_box(output, *svpi_path, payload_size, payload_hash,
                          compact_box_size_limit);
  }
  return output.good();
}

}  // namespace svp::package::detail
