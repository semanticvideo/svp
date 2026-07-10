#include "query_test_registry.hpp"
#include "fixtures/semantic_package_fixture.hpp"

#include "svp/package/embedded_svpi.hpp"
#include "svp/query/query_ops.hpp"
#include "svp/query/query_reader.hpp"
#include "svp/query/traversal.hpp"

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

void write_ftyp(std::ostream& output, const char* brand) {
  write_u32(output, 20);
  output.write("ftyp", 4);
  output.write(brand, 4);
  write_u32(output, 0);
  output.write(brand, 4);
}

void embedded_package_queries_match_standalone_queries() {
  const auto package_path = create_semantic_test_package();
  const auto root = package_path.parent_path();
  const auto container_path = root / "source-container.data";
  const auto embedded_path = root / "embedded-query.mov";
  {
    std::ofstream output(container_path, std::ios::binary);
    write_ftyp(output, "qt  ");
    write_empty_box(output, "moov");
    write_empty_box(output, "mdat");
  }

  const auto embedded = svp::package::embed_svpi_in_iso_bmff(
      container_path, package_path, embedded_path);
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

  svp::query::TraversalOptions traversal_options;
  traversal_options.start_id = "word_000001";
  traversal_options.max_depth = 3;
  const auto standalone_traversal = svp::query::traverse_relationships(
      package_path, traversal_options);
  const auto embedded_traversal = svp::query::traverse_relationships(
      embedded_path, traversal_options);
  require(standalone_traversal.error_message ==
          embedded_traversal.error_message);
  require(svp::query::traversal_result_to_json(standalone_traversal) ==
          svp::query::traversal_result_to_json(embedded_traversal));

  const auto standalone_health =
      svp::query::compute_graph_health(package_path);
  const auto embedded_health =
      svp::query::compute_graph_health(embedded_path);
  require(svp::query::graph_health_to_json(standalone_health) ==
          svp::query::graph_health_to_json(embedded_health));
}

REGISTER_QUERY_TEST(embedded_package_queries_match_standalone_queries)

}  // namespace
