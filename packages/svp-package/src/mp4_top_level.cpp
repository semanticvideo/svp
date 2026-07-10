#include "mp4_top_level.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <system_error>

namespace svp::package::detail {
namespace {

bool checked_add(std::uint64_t left, std::uint64_t right,
                 std::uint64_t& result) noexcept {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return false;
  }
  result = left + right;
  return true;
}

EmbeddedSvpiIssue issue(EmbeddedSvpiIssueCode code, std::uint64_t offset,
                        std::string message) {
  return {.code = code, .offset = offset, .message = std::move(message)};
}

}  // namespace

std::uint32_t read_be32(const std::uint8_t* bytes) noexcept {
  return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
         (static_cast<std::uint32_t>(bytes[1]) << 16U) |
         (static_cast<std::uint32_t>(bytes[2]) << 8U) |
         static_cast<std::uint32_t>(bytes[3]);
}

std::uint64_t read_be64(const std::uint8_t* bytes) noexcept {
  return (static_cast<std::uint64_t>(read_be32(bytes)) << 32U) |
         read_be32(bytes + 4);
}

void write_be16(std::uint8_t* bytes, std::uint16_t value) noexcept {
  bytes[0] = static_cast<std::uint8_t>(value >> 8U);
  bytes[1] = static_cast<std::uint8_t>(value);
}

void write_be32(std::uint8_t* bytes, std::uint32_t value) noexcept {
  bytes[0] = static_cast<std::uint8_t>(value >> 24U);
  bytes[1] = static_cast<std::uint8_t>(value >> 16U);
  bytes[2] = static_cast<std::uint8_t>(value >> 8U);
  bytes[3] = static_cast<std::uint8_t>(value);
}

void write_be64(std::uint8_t* bytes, std::uint64_t value) noexcept {
  write_be32(bytes, static_cast<std::uint32_t>(value >> 32U));
  write_be32(bytes + 4, static_cast<std::uint32_t>(value));
}

bool read_exact_at(std::ifstream& input, std::uint64_t offset,
                   void* destination, std::size_t size) {
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
      size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
    return false;
  }
  input.clear();
  input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!input) {
    return false;
  }
  input.read(static_cast<char*>(destination), static_cast<std::streamsize>(size));
  return input.good() || static_cast<std::size_t>(input.gcount()) == size;
}

TopLevelScan scan_top_level_boxes(const std::filesystem::path& path) {
  TopLevelScan scan;
  std::error_code error;
  scan.file_size = std::filesystem::file_size(path, error);
  if (error) {
    scan.issues.push_back(issue(EmbeddedSvpiIssueCode::input_unreadable, 0,
                                "Unable to determine input file size: " + error.message()));
    return scan;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    scan.issues.push_back(issue(EmbeddedSvpiIssueCode::input_unreadable, 0,
                                "Unable to open input file."));
    return scan;
  }
  scan.readable = true;

  std::uint64_t offset = 0;
  while (offset < scan.file_size) {
    const std::uint64_t remaining = scan.file_size - offset;
    if (remaining < 8) {
      scan.issues.push_back(issue(EmbeddedSvpiIssueCode::invalid_box_structure, offset,
                                  "Truncated top-level ISO BMFF box header."));
      return scan;
    }

    std::array<std::uint8_t, 16> header{};
    if (!read_exact_at(input, offset, header.data(), 8)) {
      scan.issues.push_back(issue(EmbeddedSvpiIssueCode::input_unreadable, offset,
                                  "Unable to read top-level ISO BMFF box header."));
      return scan;
    }
    scan.bytes_read += 8;

    TopLevelBox box;
    box.offset = offset;
    std::memcpy(box.type.data(), header.data() + 4, box.type.size());
    const auto compact_size = read_be32(header.data());
    box.header_size = 8;

    if (compact_size == 0) {
      box.size = remaining;
      box.extends_to_eof = true;
    } else if (compact_size == 1) {
      if (remaining < 16 || !read_exact_at(input, offset + 8, header.data() + 8, 8)) {
        scan.issues.push_back(issue(EmbeddedSvpiIssueCode::invalid_box_structure, offset,
                                    "Truncated extended-size ISO BMFF box header."));
        return scan;
      }
      scan.bytes_read += 8;
      box.header_size = 16;
      box.size = read_be64(header.data() + 8);
      if (box.size < box.header_size) {
        scan.issues.push_back(issue(EmbeddedSvpiIssueCode::invalid_box_structure, offset,
                                    "Extended-size box is smaller than its header."));
        return scan;
      }
    } else {
      box.size = compact_size;
      if (box.size < box.header_size) {
        scan.issues.push_back(issue(EmbeddedSvpiIssueCode::invalid_box_structure, offset,
                                    "Box is smaller than its header."));
        return scan;
      }
    }

    std::uint64_t box_end = 0;
    if (!checked_add(offset, box.size, box_end) || box_end > scan.file_size) {
      scan.issues.push_back(issue(EmbeddedSvpiIssueCode::invalid_box_structure, offset,
                                  "Top-level box extends beyond the input file."));
      return scan;
    }

    if (box.type == std::array<char, 4>{'u', 'u', 'i', 'd'}) {
      if (box.size < box.header_size + 16) {
        scan.issues.push_back(issue(EmbeddedSvpiIssueCode::truncated_uuid_box, offset,
                                    "UUID box is too small to contain a user type."));
        return scan;
      }
      box.header_size += 16;
    }

    scan.boxes.push_back(box);
    offset = box_end;
    if (box.extends_to_eof) {
      break;
    }
  }

  scan.valid = offset == scan.file_size;
  return scan;
}

}  // namespace svp::package::detail
