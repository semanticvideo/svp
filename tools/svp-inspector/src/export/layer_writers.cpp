#include "layer_writers.hpp"

#include "export_error.hpp"
#include "jsonl_line_reader.hpp"
#include "jsonl_record.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace package_export {
namespace {

// I/O batching only: bytes copied per read when mirroring an entry.
constexpr std::size_t kCopyChunkBytes = std::size_t{1} << 20U;

}  // namespace

void export_jsonl_layer(const PackageReader& reader, const PlannedLayer& layer,
                        const ReferenceResolver& resolver,
                        OutputTransaction& output, LayerResult& result) {
  const auto& entry = layer.entry;
  result.rules = rules_for_layer(entry.name);
  auto file = output.create_file(layer.files.at(0).path);
  JsonlLineReader lines{reader.open(entry), entry.name};
  std::string_view text;
  std::uint64_t line = 0;
  std::uint64_t record_count = 0;
  while (lines.next(text, line)) {
    const auto record = parse_record(text, entry.name, line);
    const auto resolutions = result.rules.empty()
                                 ? nlohmann::json::object()
                                 : resolver.resolve(record, result.rules);
    if (resolutions.empty()) {
      file.write(text);
    } else {
      file.write(append_export_member(text, record, resolutions));
    }
    file.write("\n");
    ++record_count;
  }
  file.close();
  result.file_sizes = {file.size_bytes()};
  result.record_count = record_count;
}

void export_json_document(const PackageReader& reader,
                          const PlannedLayer& layer, OutputTransaction& output,
                          LayerResult& result) {
  const auto& entry = layer.entry;
  const auto content = reader.read_whole(entry, kMaxRecordBytes);
  if (!nlohmann::json::accept(content)) {
    throw ExportError(ExportErrorCode::malformed_document,
                      "A JSON package entry is not valid JSON.",
                      entry_details(entry.name));
  }
  auto file = output.create_file(layer.files.at(0).path);
  file.write(content);
  file.close();
  result.file_sizes = {file.size_bytes()};
}

void export_file_bytes(const PackageReader& reader, const PlannedLayer& layer,
                       OutputTransaction& output, LayerResult& result) {
  auto file = output.create_file(layer.files.at(0).path);
  auto stream = reader.open(layer.entry);
  std::vector<char> buffer(kCopyChunkBytes);
  while (const auto count = stream.read(buffer.data(), buffer.size())) {
    file.write(std::string_view{buffer.data(), count});
  }
  file.close();
  result.file_sizes = {file.size_bytes()};
}

}  // namespace package_export
