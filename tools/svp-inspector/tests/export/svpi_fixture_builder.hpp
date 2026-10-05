#pragma once

#include "export_test_support.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace export_test {

// Values the synthetic SVPI holds, so tests can check the export against
// what was written rather than against itself.
struct SvpiFixtureFacts {
  std::vector<std::uint16_t> depth_block_0;
  std::vector<std::uint16_t> depth_block_1;
  std::vector<float> embedding_vector;
  std::uint64_t depth_block_1_offset = 0;
  std::string crop_bytes;
  std::string extension_bytes;
};

struct SvpiFixture {
  std::vector<ZipEntrySpec> entries;
  SvpiFixtureFacts facts;
};

// An SVPI that passes validate_svpi_package and holds every representation
// the export handles: JSONL layers with references to resolve, JSON
// documents, SVPB depth and embedding streams, an empty mask stream, an
// evidence crop image, the SQLite index, and an unknown extension file.
[[nodiscard]] SvpiFixture make_svpi_fixture();

[[nodiscard]] const std::string& entry_content(
    const std::vector<ZipEntrySpec>& entries, std::string_view name);
void set_entry(std::vector<ZipEntrySpec>& entries, std::string_view name,
               std::string content);
void add_entry(std::vector<ZipEntrySpec>& entries, std::string name,
               std::string content);

}  // namespace export_test
