#include "jsonl_line_reader.hpp"

#include "export_error.hpp"

#include <utility>

namespace package_export {
namespace {

// I/O batching only: bytes requested from the ZIP stream per read.
constexpr std::size_t kReadChunkBytes = std::size_t{256} << 10U;

bool is_json_whitespace(char character) noexcept {
  return character == ' ' || character == '\t' || character == '\r' ||
         character == '\n';
}

}  // namespace

std::string_view trim_json_whitespace(std::string_view text) {
  while (!text.empty() && is_json_whitespace(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && is_json_whitespace(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

JsonlLineReader::JsonlLineReader(PackageReader::EntryStream stream,
                                 std::string entry)
    : stream_(std::move(stream)), entry_(std::move(entry)) {}

bool JsonlLineReader::fill() {
  if (end_of_entry_) {
    return false;
  }
  chunk_.resize(kReadChunkBytes);
  const auto count = stream_.read(chunk_.data(), chunk_.size());
  chunk_.resize(count);
  position_ = 0;
  if (count == 0) {
    end_of_entry_ = true;
    return false;
  }
  return true;
}

bool JsonlLineReader::next(std::string_view& record, std::uint64_t& line) {
  while (true) {
    line_.clear();
    bool have_line = false;
    while (!have_line) {
      if (position_ >= chunk_.size() && !fill()) {
        break;
      }
      const auto newline = chunk_.find('\n', position_);
      const auto end = newline == std::string::npos ? chunk_.size() : newline;
      if (line_.size() + (end - position_) > kMaxRecordBytes) {
        throw ExportError(ExportErrorCode::record_too_large,
                          "A JSONL record is larger than the export holds in "
                          "memory for one record.",
                          record_details(entry_, line_number_ + 1));
      }
      line_.append(chunk_, position_, end - position_);
      position_ = end;
      if (newline != std::string::npos) {
        ++position_;
        have_line = true;
      }
    }
    if (!have_line && line_.empty()) {
      return false;  // End of entry with no partial last line.
    }
    ++line_number_;
    const auto trimmed = trim_json_whitespace(line_);
    if (trimmed.empty()) {
      if (!have_line) {
        return false;
      }
      continue;
    }
    record = trimmed;
    line = line_number_;
    return true;
  }
}

}  // namespace package_export
