#pragma once

#include "package_reader.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace package_export {

// The largest single JSON value (one JSONL record, or one JSON document) the
// export holds in memory at once. Memory use is this text plus its parsed
// form, so the bound keeps a hostile package from exhausting memory, while
// staying orders of magnitude above the records SVP writers produce (the
// largest are processor provenance records of a few hundred KiB).
inline constexpr std::uint64_t kMaxRecordBytes = std::uint64_t{64} << 20U;

// Splits a streamed JSONL entry into records. A record is a non-blank line
// with surrounding JSON whitespace removed (RFC 8259: space, tab, CR, LF).
class JsonlLineReader {
 public:
  JsonlLineReader(PackageReader::EntryStream stream, std::string entry);

  // Advances to the next record. `record` stays valid until the next call;
  // `line` is its 1-based line number in the entry. Returns false at the end.
  // Throws ExportError(record_too_large) for a line over kMaxRecordBytes.
  [[nodiscard]] bool next(std::string_view& record, std::uint64_t& line);

 private:
  bool fill();

  PackageReader::EntryStream stream_;
  std::string entry_;
  std::string chunk_;
  std::size_t position_ = 0;
  std::string line_;
  std::uint64_t line_number_ = 0;
  bool end_of_entry_ = false;
};

// Removes JSON whitespace from both ends.
[[nodiscard]] std::string_view trim_json_whitespace(std::string_view text);

}  // namespace package_export
