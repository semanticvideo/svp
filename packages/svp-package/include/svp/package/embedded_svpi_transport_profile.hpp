#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace svp::package {

inline constexpr std::string_view kEmbeddedSvpiTransportProfileName =
    "Embedded SVPI Transport for ISO Base Media File Format, Version 1";
inline constexpr std::string_view kEmbeddedSvpiTransportUuidDerivationName =
    "https://semanticvideo.org/spec/svpi-embedded-mp4/v1";
inline constexpr std::string_view kEmbeddedSvpiTransportUuidText =
    "e2b6a23c-22ca-5636-b165-991208c837f1";
inline constexpr std::array<std::uint8_t, 16> kEmbeddedSvpiTransportUuid{
    0xe2, 0xb6, 0xa2, 0x3c, 0x22, 0xca, 0x56, 0x36,
    0xb1, 0x65, 0x99, 0x12, 0x08, 0xc8, 0x37, 0xf1,
};

inline constexpr std::array<std::uint8_t, 8> kEmbeddedSvpiEnvelopeMagic{
    'S', 'V', 'P', 'I', 'M', 'P', '4', '\0',
};
inline constexpr std::uint16_t kEmbeddedSvpiProfileVersion = 1;
inline constexpr std::uint16_t kEmbeddedSvpiEnvelopeSize = 64;
inline constexpr std::uint32_t kEmbeddedSvpiSupportedFlags = 0;
inline constexpr std::size_t kEmbeddedSvpiHashSize = 32;
inline constexpr std::size_t kEmbeddedSvpiReservedSize = 8;
inline constexpr std::uint64_t kIsoBmffMaxCompactBoxSize = 0xffffffffULL;

}  // namespace svp::package
