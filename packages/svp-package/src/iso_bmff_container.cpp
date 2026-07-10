#include "svp/package/iso_bmff_container.hpp"

#include "iso_bmff_container_internal.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <ranges>
#include <string_view>

namespace svp::package {
namespace {

// Real ftyp brand tables are small. This bound prevents an untrusted ftyp
// size from driving an arbitrary allocation while leaving ample evolution room.
constexpr std::uint64_t kMaximumCompatibleBrands = 64;
constexpr std::array<std::uint8_t, 4> kFtypType{'f', 't', 'y', 'p'};

bool has_brand(const IsoBmffContainerInfo& info, std::string_view brand) {
  return info.major_brand == brand || std::ranges::any_of(
      info.compatible_brands,
      [&](const std::string& candidate) { return candidate == brand; });
}

bool is_iso_brand(std::string_view brand) {
  return brand == "isom" || brand == "iso2" || brand == "iso3" ||
         brand == "iso4" || brand == "iso5" || brand == "iso6" ||
         brand == "iso7" || brand == "iso8" || brand == "iso9";
}

IsoBmffContainerKind classify_brand(const IsoBmffContainerInfo& info) {
  if (has_brand(info, "qt  ")) {
    return IsoBmffContainerKind::quicktime;
  }
  for (const auto brand : {"M4A ", "M4B ", "M4P "}) {
    if (has_brand(info, brand)) {
      return IsoBmffContainerKind::m4a;
    }
  }
  for (const auto brand : {"M4V ", "M4VH", "M4VP"}) {
    if (has_brand(info, brand)) {
      return IsoBmffContainerKind::m4v;
    }
  }
  if (is_iso_brand(info.major_brand) ||
      std::ranges::any_of(info.compatible_brands, is_iso_brand)) {
    return IsoBmffContainerKind::mp4;
  }
  for (const auto brand : {"mp41", "mp42", "avc1", "dash", "cmfc", "cmfs"}) {
    if (has_brand(info, brand)) {
      return IsoBmffContainerKind::mp4;
    }
  }
  return IsoBmffContainerKind::unsupported_derivative;
}

}  // namespace

namespace detail {

IsoBmffContainerInfo classify_iso_bmff_container(
    const std::filesystem::path& path,
    const TopLevelScan& scan) {
  IsoBmffContainerInfo info;
  if (!scan.readable) {
    info.diagnostic = "Unable to read the candidate ISO BMFF container.";
    return info;
  }
  std::ifstream signature_input(path, std::ios::binary);
  std::array<std::uint8_t, 8> signature{};
  if (!read_exact_at(signature_input, 0, signature.data(), signature.size()) ||
      !std::equal(signature.begin() + 4, signature.end(), kFtypType.begin())) {
    info.diagnostic = "The first top-level box is not ftyp.";
    return info;
  }
  info.bytes_read += signature.size();
  info.signature_present = true;
  if (!scan.valid) {
    info.diagnostic = "Malformed top-level ISO BMFF box structure.";
    return info;
  }
  info.structure_valid = true;
  if (scan.boxes.empty() ||
      scan.boxes.front().type != std::array<char, 4>{'f', 't', 'y', 'p'}) {
    info.structure_valid = false;
    info.diagnostic = "The ftyp box could not be resolved from the top-level structure.";
    return info;
  }

  const auto& ftyp = scan.boxes.front();
  if (ftyp.size < ftyp.header_size + 8 ||
      (ftyp.size - ftyp.header_size - 8) % 4 != 0) {
    info.structure_valid = false;
    info.diagnostic = "The ftyp box has an invalid brand table size.";
    return info;
  }

  std::ifstream input(path, std::ios::binary);
  std::array<std::uint8_t, 8> fixed{};
  if (!read_exact_at(input, ftyp.offset + ftyp.header_size,
                     fixed.data(), fixed.size())) {
    info.structure_valid = false;
    info.diagnostic = "Unable to read the ftyp major brand and minor version.";
    return info;
  }
  info.bytes_read += fixed.size();
  info.major_brand.assign(reinterpret_cast<const char*>(fixed.data()), 4);
  info.minor_version = read_be32(fixed.data() + 4);

  const auto compatible_count =
      (ftyp.size - ftyp.header_size - fixed.size()) / 4;
  if (compatible_count > kMaximumCompatibleBrands) {
    info.structure_valid = false;
    info.diagnostic = "The ftyp compatible-brand table exceeds the supported bound.";
    return info;
  }
  info.compatible_brands.reserve(static_cast<std::size_t>(compatible_count));
  for (std::uint64_t index = 0; index < compatible_count; ++index) {
    std::array<char, 4> brand{};
    if (!read_exact_at(input, ftyp.offset + ftyp.header_size + fixed.size() +
                                  index * brand.size(),
                       brand.data(), brand.size())) {
      info.structure_valid = false;
      info.diagnostic = "Unable to read the complete ftyp brand table.";
      return info;
    }
    info.bytes_read += brand.size();
    info.compatible_brands.emplace_back(brand.data(), brand.size());
  }

  info.kind = classify_brand(info);
  info.supported = info.kind != IsoBmffContainerKind::unsupported_derivative;
  if (!info.supported) {
    info.diagnostic = "The ISO BMFF brand family is not supported for SVPI embedding.";
  }
  return info;
}

}  // namespace detail

IsoBmffContainerInfo inspect_iso_bmff_container(
    const std::filesystem::path& path) {
  const auto scan = detail::scan_top_level_boxes(path);
  return detail::classify_iso_bmff_container(path, scan);
}

const char* to_string(IsoBmffContainerKind kind) noexcept {
  switch (kind) {
    case IsoBmffContainerKind::unknown: return "unknown";
    case IsoBmffContainerKind::mp4: return "mp4";
    case IsoBmffContainerKind::quicktime: return "quicktime";
    case IsoBmffContainerKind::m4v: return "m4v";
    case IsoBmffContainerKind::m4a: return "m4a";
    case IsoBmffContainerKind::unsupported_derivative:
      return "unsupported_iso_bmff_derivative";
  }
  return "unknown";
}

}  // namespace svp::package
