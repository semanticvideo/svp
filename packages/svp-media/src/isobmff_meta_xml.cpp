#include "isobmff_meta_xml.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <string_view>

namespace svp::media {
namespace {

// A file-level metadata box is a few kilobytes of tables and XML; anything
// past this is not camera metadata, and is never read into memory.
constexpr std::uint64_t kMaxMetaBoxBytes = 4ULL * 1024 * 1024;

// ISO/IEC 14496-12 4.2: a box header is a 32-bit size and a four-character
// type; size 1 means a 64-bit size follows, size 0 means "to the end of the
// file". A full box (`meta`, `xml `) adds 4 bytes of version and flags.
constexpr std::uint64_t kBoxHeaderBytes = 8;
constexpr std::uint64_t kLargeSizeBytes = 8;
constexpr std::uint64_t kFullBoxBytes = 4;

struct BoxHeader {
  std::uint64_t size = 0;  // the whole box, header included
  std::uint64_t header_size = 0;
  std::array<char, 4> type{};
};

std::uint64_t big_endian(const unsigned char* bytes, std::size_t count) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < count; ++i) {
    value = (value << 8) | bytes[i];
  }
  return value;
}

std::optional<BoxHeader> read_header(std::istream& in, std::uint64_t at,
                                     std::uint64_t limit) {
  if (at + kBoxHeaderBytes > limit) {
    return std::nullopt;
  }
  unsigned char raw[kBoxHeaderBytes + kLargeSizeBytes];
  in.seekg(static_cast<std::streamoff>(at));
  if (!in.read(reinterpret_cast<char*>(raw), kBoxHeaderBytes)) {
    return std::nullopt;
  }
  BoxHeader header;
  header.header_size = kBoxHeaderBytes;
  header.size = big_endian(raw, 4);
  for (std::size_t i = 0; i < 4; ++i) {
    header.type[i] = static_cast<char>(raw[4 + i]);
  }
  if (header.size == 1) {
    if (at + kBoxHeaderBytes + kLargeSizeBytes > limit ||
        !in.read(reinterpret_cast<char*>(raw + kBoxHeaderBytes), kLargeSizeBytes)) {
      return std::nullopt;
    }
    header.size = big_endian(raw + kBoxHeaderBytes, kLargeSizeBytes);
    header.header_size += kLargeSizeBytes;
  } else if (header.size == 0) {
    header.size = limit - at;
  }
  if (header.size < header.header_size || header.size > limit - at) {
    return std::nullopt;
  }
  return header;
}

bool is_type(const BoxHeader& header, std::string_view type) {
  return std::string_view(header.type.data(), header.type.size()) == type;
}

std::optional<std::string> xml_in_meta(std::istream& in, std::uint64_t at,
                                       const BoxHeader& meta) {
  if (meta.size > kMaxMetaBoxBytes || meta.size < meta.header_size + kFullBoxBytes) {
    return std::nullopt;
  }
  const std::uint64_t end = at + meta.size;
  std::uint64_t child = at + meta.header_size + kFullBoxBytes;
  while (child < end) {
    const auto header = read_header(in, child, end);
    if (!header) {
      return std::nullopt;
    }
    if (is_type(*header, "xml ")) {
      if (header->size < header->header_size + kFullBoxBytes) {
        return std::nullopt;
      }
      const std::uint64_t length = header->size - header->header_size - kFullBoxBytes;
      std::string xml(length, '\0');
      in.seekg(static_cast<std::streamoff>(child + header->header_size + kFullBoxBytes));
      if (!in.read(xml.data(), static_cast<std::streamsize>(length))) {
        return std::nullopt;
      }
      // The XML may be stored with a trailing NUL terminator.
      while (!xml.empty() && xml.back() == '\0') {
        xml.pop_back();
      }
      return xml;
    }
    child += header->size;
  }
  return std::nullopt;
}

}  // namespace

std::optional<std::string> read_isobmff_meta_xml(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  in.seekg(0, std::ios::end);
  const auto file_size = static_cast<std::uint64_t>(in.tellg());
  // An ISO BMFF file starts with its `ftyp` box; anything else (MXF, raw
  // streams) is not walked as boxes at all.
  const auto first = read_header(in, 0, file_size);
  if (!first || !is_type(*first, "ftyp")) {
    return std::nullopt;
  }
  std::uint64_t at = 0;
  while (at < file_size) {
    const auto header = read_header(in, at, file_size);
    if (!header) {
      return std::nullopt;
    }
    if (is_type(*header, "meta")) {
      return xml_in_meta(in, at, *header);
    }
    at += header->size;
  }
  return std::nullopt;
}

}  // namespace svp::media
