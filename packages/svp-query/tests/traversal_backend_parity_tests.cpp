#include "query_test_registry.hpp"
#include "query_test_io.hpp"
#include "svp/query/traversal.hpp"
#include "svp/package/package_writer.hpp"
#include "svp/package/package_layout.hpp"

#include <sqlite3.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

void test_traversal_jsonl_sqlite_parity() {
  // Create a package with BOTH index.sqlite (containing a relationships table)
  // and relationships.jsonl with the same data. Then verify:
  // 1. SQLite-backed traversal produces the same edges/nodes as JSONL.
  // 2. Graph health reports the correct backend.
  //
  // Since load_relationship_edges tries SQLite first and falls back to JSONL,
  // we create two packages: one with both (uses SQLite) and one with only
  // JSONL (uses JSONL), then compare results.

  const auto root = std::filesystem::temp_directory_path() / "svp-query-parity-tests";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto staging = root / "staging";
  const auto pkg_with_sqlite = root / "test_parity_sqlite.svp";
  const auto pkg_jsonl_only = root / "test_parity_jsonl.svp";
  const auto source_path = root / "source.mp4";

  write_file(source_path, "mock media\n");

  nlohmann::json manifest = {
      {"svp_version", "1.0-rc.2"},
      {"package_id", "svp_query_parity_pkg"},
      {"created_utc", "2026-06-20T00:00:00Z"},
      {"primary_media_id", "media_000001"},
      {"timebase", {{"unit", "microseconds"}, {"origin", "primary_presentation_start"}}},
      {"canonical_analysis_raster", {{"width", 640}, {"height", 360}}}
  };

  // Define the same relationships for both backends.
  std::vector<nlohmann::json> relationships = {
      {{"id", "rel_001"}, {"type", "word_spoken_by"}, {"source_id", "word_000001"},
       {"target_id", "speaker_0001"}, {"start_us", 0}, {"end_us", 500000},
       {"confidence", 0.95}, {"processor_id", "proc_test"}},
      {{"id", "rel_002"}, {"type", "word_spoken_by"}, {"source_id", "word_000002"},
       {"target_id", "speaker_0001"}, {"start_us", 500000}, {"end_us", 1000000},
       {"confidence", 0.95}, {"processor_id", "proc_test"}},
      {{"id", "rel_003"}, {"type", "observation_in_region"},
       {"source_id", "text_obs_000001"}, {"target_id", "text_region_000001"},
       {"start_us", 1000000}, {"end_us", 2000000},
       {"confidence", 0.9}, {"processor_id", "proc_test"}},
  };

  // Helper to write minimal staging files.
  auto write_minimal_staging = [&](const std::filesystem::path& stg) {
    write_jsonl(stg / "transcript" / "words.jsonl", {
        {{"id", "word_000001"}, {"text", "hello"}, {"normalized_text", "hello"},
         {"start_us", 0}, {"end_us", 500000}, {"speaker_id", "speaker_0001"}},
        {{"id", "word_000002"}, {"text", "world"}, {"normalized_text", "world"},
         {"start_us", 500000}, {"end_us", 1000000}, {"speaker_id", "speaker_0001"}}
    });
    write_jsonl(stg / "transcript" / "speakers.jsonl", {
        {{"id", "speaker_0001"}, {"display_name", "Speaker 1"},
         {"total_speech_us", 1000000}, {"confidence", 0.91}}
    });
    write_jsonl(stg / "transcript" / "speaker_segments.jsonl", {});
    write_jsonl(stg / "transcript" / "speech_regions.jsonl", {});
    write_jsonl(stg / "timeline" / "frames.jsonl", {});
    write_jsonl(stg / "timeline" / "shots.jsonl", {
        {{"id", "shot_000001"}, {"start_us", 0}, {"end_us", 15000000}}
    });
    write_jsonl(stg / "timeline" / "scenes.jsonl", {
        {{"id", "scene_000001"}, {"start_us", 0}, {"end_us", 30000000},
         {"shot_ids", {"shot_000001"}}}
    });
    write_jsonl(stg / "entities" / "entities.jsonl", {});
    write_jsonl(stg / "entities" / "entity_tracks.jsonl", {});
    std::filesystem::create_directories(stg / "spatial");
    write_file(stg / "spatial" / "regions.jsonl", "");
    write_file(stg / "spatial" / "masks.index.jsonl", "");
    write_file(stg / "spatial" / "depth.index.jsonl", "");
    write_jsonl(stg / "text" / "text_regions.jsonl", {
        {{"text_region_id", "text_region_000001"},
         {"observation_type", "text_detection"},
         {"start_us", 1000000}, {"end_us", 2000000},
         {"shot_id", "shot_000001"}, {"scene_id", "scene_000001"}}
    });
    write_jsonl(stg / "text" / "text_observations.jsonl", {
        {{"text_observation_id", "text_obs_000001"},
         {"text_region_id", "text_region_000001"},
         {"observation_type", "text_recognition"},
         {"raw_text", "SALE"}, {"normalized_text", "sale"},
         {"confidence", 0.902}}
    });
    write_jsonl(stg / "text" / "numeric_values.jsonl", {});
    write_jsonl(stg / "text" / "evidence_crops.jsonl", {});
    write_json(stg / "text" / "text_absence.json", {
        {"schema_version", "svp-text-absence-v1"},
        {"ocr_required", true}, {"ocr_completed", true},
        {"text_region_count", 1}, {"text_observation_count", 1},
        {"reason", "text_detected"}
    });
    write_jsonl(stg / "colors" / "color_observations.jsonl", {});
    write_json(stg / "colors" / "color_summary.json", {
        {"schema_version", "svp-color-summary-v1"},
        {"color_observation_count", 0},
        {"color_space", "svp_oklch_v1"},
        {"color_bucket_registry_version", "svp-color-buckets-v1"}
    });
    write_json(stg / "colors" / "color_absence.json", {
        {"schema_version", "svp-color-absence-v1"},
        {"color_required", true}, {"color_completed", true}
    });
    write_json(stg / "embeddings" / "embedding_sets.json", {{"schema_version", "svp-embedding-sets-v1"}});
    write_file(stg / "embeddings" / "embeddings.index.jsonl", "");
    std::filesystem::create_directories(stg / "provenance");
    write_json(stg / "provenance" / "build.json", {{"build_id", "test_build"}});
    write_jsonl(stg / "provenance" / "processors.jsonl", {});
    write_jsonl(stg / "provenance" / "input_hashes.jsonl", {});
    write_jsonl(stg / "provenance" / "model_hashes.jsonl", {});
    write_json(stg / "provenance" / "validation.json", {
        {"status", "valid"}, {"core_status", "valid"}, {"authenticity_status", "valid"}
    });
    std::filesystem::create_directories(stg / "media" / "original");
    std::filesystem::create_directories(stg / "media" / "audio");
  };

  // --- Package 1: JSONL only (no index.sqlite) ---
  {
    const auto stg = staging / "jsonl_only";
    write_minimal_staging(stg);
    write_jsonl(stg / "relationships" / "relationships.jsonl", relationships);
    std::filesystem::create_directories(stg / "index");
    write_json(stg / "index" / "index_manifest.json", {
        {"index_schema_version", "svp-index-v1"},
        {"sqlite_file", "index.sqlite"},
        {"logical_row_stream_version", "1"},
        {"table_count", 6}, {"row_count", 10}
    });
    bool ok = svp::package::write_package_skeleton(pkg_jsonl_only, stg, source_path, manifest);
    assert(ok);
  }

  // --- Package 2: Both JSONL and index.sqlite ---
  {
    const auto stg = staging / "with_sqlite";
    write_minimal_staging(stg);
    write_jsonl(stg / "relationships" / "relationships.jsonl", relationships);

    // Create a real SQLite database with a relationships table.
    const auto sqlite_path = stg / "index" / "index.sqlite";
    std::filesystem::create_directories(stg / "index");

    sqlite3* db = nullptr;
    assert(sqlite3_open(sqlite_path.string().c_str(), &db) == SQLITE_OK);

    const char* create_sql =
        "CREATE TABLE relationships ("
        "  relationship_id TEXT,"
        "  relationship_type TEXT,"
        "  relationship_class TEXT,"
        "  source_id TEXT,"
        "  target_id TEXT,"
        "  start_us INTEGER,"
        "  end_us INTEGER,"
        "  confidence REAL)";
    assert(sqlite3_exec(db, create_sql, nullptr, nullptr, nullptr) == SQLITE_OK);

    for (const auto& rel : relationships) {
      const std::string sql =
          std::string("INSERT INTO relationships VALUES (") +
          "'" + rel["id"].get<std::string>() + "', " +
          "'" + rel["type"].get<std::string>() + "', " +
          "'support', " +
          "'" + rel["source_id"].get<std::string>() + "', " +
          "'" + rel["target_id"].get<std::string>() + "', " +
          std::to_string(rel["start_us"].get<std::int64_t>()) + ", " +
          std::to_string(rel["end_us"].get<std::int64_t>()) + ", " +
          std::to_string(rel["confidence"].get<double>()) + ")";
      assert(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    }
    sqlite3_close(db);

    write_json(stg / "index" / "index_manifest.json", {
        {"index_schema_version", "svp-index-v1"},
        {"sqlite_file", "index.sqlite"},
        {"logical_row_stream_version", "1"},
        {"table_count", 6}, {"row_count", 10}
    });
    bool ok = svp::package::write_package_skeleton(pkg_with_sqlite, stg, source_path, manifest);
    assert(ok);
  }

  // Verify both packages have the expected entries.
  const auto layout_sqlite = svp::package::read_package_layout(pkg_with_sqlite);
  assert(layout_sqlite.has_value());
  assert(layout_sqlite.value().has_entry("index/index.sqlite"));
  assert(layout_sqlite.value().has_entry("relationships/relationships.jsonl"));

  const auto layout_jsonl = svp::package::read_package_layout(pkg_jsonl_only);
  assert(layout_jsonl.has_value());
  assert(!layout_jsonl.value().has_entry("index/index.sqlite"));
  assert(layout_jsonl.value().has_entry("relationships/relationships.jsonl"));

  // Traverse both packages with the same options.
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.max_depth = 3;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.limit = 100;

  auto result_sqlite = svp::query::traverse_relationships(pkg_with_sqlite, opts);
  auto result_jsonl = svp::query::traverse_relationships(pkg_jsonl_only, opts);

  // Both should succeed.
  assert(result_sqlite.error_message.empty());
  assert(result_jsonl.error_message.empty());

  // Both should have the same number of edges.
  assert(result_sqlite.edges.size() == result_jsonl.edges.size());
  assert(result_sqlite.edges.size() > 0);

  // Both should have the same visited node count.
  assert(result_sqlite.visited_node_count == result_jsonl.visited_node_count);

  // Compare edge-by-edge (sorted by relationship ID).
  auto compare_edges = [](svp::query::TraversalResult& r) {
    std::sort(r.edges.begin(), r.edges.end(),
              [](const svp::query::TraversalEdge& a, const svp::query::TraversalEdge& b) {
                return a.relationship_id < b.relationship_id;
              });
  };
  compare_edges(result_sqlite);
  compare_edges(result_jsonl);

  for (std::size_t i = 0; i < result_sqlite.edges.size(); ++i) {
    assert(result_sqlite.edges[i].relationship_id == result_jsonl.edges[i].relationship_id);
    assert(result_sqlite.edges[i].relationship_type == result_jsonl.edges[i].relationship_type);
    assert(result_sqlite.edges[i].source_id == result_jsonl.edges[i].source_id);
    assert(result_sqlite.edges[i].target_id == result_jsonl.edges[i].target_id);
  }

  // Verify graph health reports the correct backend.
  auto health_sqlite = svp::query::compute_graph_health(pkg_with_sqlite);
  assert(health_sqlite.backend_used == svp::query::TraversalBackend::sqlite_index);
  assert(health_sqlite.index_available);
  assert(health_sqlite.total_edges > 0);

  auto health_jsonl = svp::query::compute_graph_health(pkg_jsonl_only);
  assert(health_jsonl.backend_used == svp::query::TraversalBackend::jsonl);
  assert(!health_jsonl.index_available);
  assert(health_jsonl.total_edges > 0);

  // Both health reports should have the same total_edges.
  assert(health_sqlite.total_edges == health_jsonl.total_edges);

  // --- Path parity ---
  // Find a path from word_000001 to speaker_0001 (direct edge via word_spoken_by).
  svp::query::TraversalOptions path_opts;
  path_opts.start_id = "word_000001";
  path_opts.target_id = "speaker_0001";
  path_opts.max_depth = 3;
  path_opts.direction = svp::query::TraversalDirection::Both;
  path_opts.limit = 100;

  auto path_sqlite = svp::query::find_shortest_path(pkg_with_sqlite, path_opts);
  auto path_jsonl = svp::query::find_shortest_path(pkg_jsonl_only, path_opts);

  assert(path_sqlite.error_message.empty());
  assert(path_jsonl.error_message.empty());
  assert(path_sqlite.path_found == path_jsonl.path_found);
  assert(path_sqlite.path_found);  // word_000001 -> speaker_0001 exists
  assert(path_sqlite.path_edges.size() == path_jsonl.path_edges.size());
  assert(path_sqlite.path_nodes.size() == path_jsonl.path_nodes.size());

  // Compare path edges
  for (std::size_t i = 0; i < path_sqlite.path_edges.size(); ++i) {
    assert(path_sqlite.path_edges[i].relationship_id ==
           path_jsonl.path_edges[i].relationship_id);
    assert(path_sqlite.path_edges[i].source_id ==
           path_jsonl.path_edges[i].source_id);
    assert(path_sqlite.path_edges[i].target_id ==
           path_jsonl.path_edges[i].target_id);
  }

  // --- Context parity ---
  // Build context for word_000001.
  auto ctx_sqlite = svp::query::build_context(pkg_with_sqlite, "word_000001", 100);
  auto ctx_jsonl = svp::query::build_context(pkg_jsonl_only, "word_000001", 100);

  assert(ctx_sqlite.error_message.empty());
  assert(ctx_jsonl.error_message.empty());
  assert(ctx_sqlite.resolved == ctx_jsonl.resolved);
  assert(ctx_sqlite.resolved);  // word_000001 exists in both packages
  assert(ctx_sqlite.context_edges.size() == ctx_jsonl.context_edges.size());
  assert(ctx_sqlite.context_nodes.size() == ctx_jsonl.context_nodes.size());

  // Compare context edges (sorted by relationship ID for determinism)
  auto sort_ctx_edges = [](svp::query::ContextResult& r) {
    std::sort(r.context_edges.begin(), r.context_edges.end(),
              [](const svp::query::TraversalEdge& a, const svp::query::TraversalEdge& b) {
                return a.relationship_id < b.relationship_id;
              });
  };
  sort_ctx_edges(ctx_sqlite);
  sort_ctx_edges(ctx_jsonl);

  for (std::size_t i = 0; i < ctx_sqlite.context_edges.size(); ++i) {
    assert(ctx_sqlite.context_edges[i].relationship_id ==
           ctx_jsonl.context_edges[i].relationship_id);
    assert(ctx_sqlite.context_edges[i].source_id ==
           ctx_jsonl.context_edges[i].source_id);
    assert(ctx_sqlite.context_edges[i].target_id ==
           ctx_jsonl.context_edges[i].target_id);
  }

  std::cout << "test_traversal_jsonl_sqlite_parity: passed\n";
}

REGISTER_QUERY_TEST(test_traversal_jsonl_sqlite_parity)
