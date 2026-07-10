#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace svp::package {

inline constexpr std::string_view kSvpiMp4ProfileName =
    "SVPI Embedded in ISO Base Media File Format / MP4, Version 1";
inline constexpr std::string_view kSvpiMp4ProfileDerivationName =
    "https://semanticvideo.org/spec/svpi-embedded-mp4/v1";
inline constexpr std::string_view kSvpiMp4UuidText =
    "e2b6a23c-22ca-5636-b165-991208c837f1";
inline constexpr std::array<std::uint8_t, 16> kSvpiMp4Uuid{
    0xe2, 0xb6, 0xa2, 0x3c, 0x22, 0xca, 0x56, 0x36,
    0xb1, 0x65, 0x99, 0x12, 0x08, 0xc8, 0x37, 0xf1,
};

inline constexpr std::array<std::uint8_t, 8> kSvpiMp4EnvelopeMagic{
    'S', 'V', 'P', 'I', 'M', 'P', '4', '\0',
};
inline constexpr std::uint16_t kSvpiMp4ProfileVersion = 1;
inline constexpr std::uint16_t kSvpiMp4EnvelopeSize = 64;
inline constexpr std::uint32_t kSvpiMp4SupportedFlags = 0;
inline constexpr std::size_t kSvpiMp4HashSize = 32;
inline constexpr std::size_t kSvpiMp4ReservedSize = 8;
inline constexpr std::uint64_t kIsoBmffMaxCompactBoxSize = 0xffffffffULL;

}  // namespace svp::package
