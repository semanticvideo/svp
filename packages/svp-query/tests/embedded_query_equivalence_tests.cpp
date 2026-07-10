#include "query_test_registry.hpp"
#include "fixtures/semantic_package_fixture.hpp"

#include "svp/package/embedded_svpi.hpp"
#include "svp/query/query_ops.hpp"
#include "svp/query/query_reader.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {

void require(bool condition) {
  if (!condition) {
    throw std::runtime_error("embedded query equivalence requirement failed");
  }
}

void write_u32(std::ostream& output, std::uint32_t value) {
  const char bytes[] = {
      static_cast<char>(value >> 24U), static_cast<char>(value >> 16U),
      static_cast<char>(value >> 8U), static_cast<char>(value)};
  output.write(bytes, sizeof(bytes));
}

void write_empty_box(std::ostream& output, const char* type) {
  write_u32(output, 8);
  output.write(type, 4);
}

void embedded_package_queries_match_standalone_queries() {
  const auto package_path = create_semantic_test_package();
  const auto root = package_path.parent_path();
  const auto mp4_path = root / "source-container.mp4";
  const auto embedded_path = root / "embedded-query.mp4";
  {
    std::ofstream output(mp4_path, std::ios::binary);
    write_empty_box(output, "ftyp");
    write_empty_box(output, "moov");
    write_empty_box(output, "mdat");
  }

  const auto embedded = svp::package::embed_svpi_in_mp4(
      mp4_path, package_path, embedded_path);
  require(embedded.success);

  const auto standalone_words =
      svp::query::read_jsonl_entry(package_path, "transcript/words.jsonl");
  const auto embedded_words =
      svp::query::read_jsonl_entry(embedded_path, "transcript/words.jsonl");
  require(standalone_words.readable && embedded_words.readable);
  require(standalone_words.records == embedded_words.records);

  const auto standalone_transcript = svp::query::transcript_summary(package_path);
  const auto embedded_transcript = svp::query::transcript_summary(embedded_path);
  require(standalone_transcript.transcript_json ==
          embedded_transcript.transcript_json);
  require(standalone_transcript.word_count_file ==
          embedded_transcript.word_count_file);

  const auto standalone_matches = svp::query::find_words(package_path, "world", 10);
  const auto embedded_matches = svp::query::find_words(embedded_path, "world", 10);
  require(standalone_matches.size() == embedded_matches.size());
  require(!standalone_matches.empty());
  require(standalone_matches.front().record == embedded_matches.front().record);

  const auto standalone_relationships =
      svp::query::relationship_summary(package_path);
  const auto embedded_relationships =
      svp::query::relationship_summary(embedded_path);
  require(standalone_relationships.total_count ==
          embedded_relationships.total_count);
  require(standalone_relationships.semantic_count ==
          embedded_relationships.semantic_count);
}

REGISTER_QUERY_TEST(embedded_package_queries_match_standalone_queries)

}  // namespace
