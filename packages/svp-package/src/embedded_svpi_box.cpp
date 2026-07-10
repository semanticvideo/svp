#include "embedded_svpi_box.hpp"

#include "isobmff_top_level.hpp"
#include "svp/package/embedded_svpi.hpp"
#include "svp/package/embedded_svpi_transport_profile.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace svp::package::detail {

std::vector<std::uint8_t> make_svpi_uuid_box_header(
    std::uint64_t svpi_payload_size) {
  const bool extended = svpi_uuid_box_requires_extended_size(svpi_payload_size);
  const std::uint64_t header_size = extended ? 32 : 24;
  constexpr std::uint64_t envelope_size = kEmbeddedSvpiEnvelopeSize;
  if (svpi_payload_size >
      std::numeric_limits<std::uint64_t>::max() - header_size - envelope_size) {
    throw std::overflow_error("SVPI UUID box size overflows uint64");
  }
  const std::uint64_t box_size = header_size + envelope_size + svpi_payload_size;

  std::vector<std::uint8_t> header(static_cast<std::size_t>(header_size));
  if (extended) {
    write_be32(header.data(), 1);
    std::copy_n("uuid", 4, reinterpret_cast<char*>(header.data() + 4));
    write_be64(header.data() + 8, box_size);
    std::copy(kEmbeddedSvpiTransportUuid.begin(), kEmbeddedSvpiTransportUuid.end(), header.begin() + 16);
  } else {
    write_be32(header.data(), static_cast<std::uint32_t>(box_size));
    std::copy_n("uuid", 4, reinterpret_cast<char*>(header.data() + 4));
    std::copy(kEmbeddedSvpiTransportUuid.begin(), kEmbeddedSvpiTransportUuid.end(), header.begin() + 8);
  }
  return header;
}

}  // namespace svp::package::detail
