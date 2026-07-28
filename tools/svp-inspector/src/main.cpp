#include "dump_command.hpp"
#include "embedded_input_controller.hpp"
#include "embedded_transport_output.hpp"
#include "inspect_output.hpp"
#include "query_output.hpp"

#include "svp/core/version.hpp"
#include "svp/package/package_summary.hpp"
#include "svp/query/traversal.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

namespace {

svp::query::TraversalDirection traversal_direction(
    const std::string& direction) {
  if (direction == "outgoing") {
    return svp::query::TraversalDirection::Outgoing;
  }
  if (direction == "incoming") {
    return svp::query::TraversalDirection::Incoming;
  }
  return svp::query::TraversalDirection::Both;
}

svp::query::TraversalOptions make_traversal_options(
    const std::string& start_id,
    const std::string& target_id,
    int max_depth,
    const std::string& direction,
    const std::string& class_filter,
    const std::string& type_filter,
    std::size_t limit,
    long long at_us,
    long long start_us,
    long long end_us) {
  svp::query::TraversalOptions options;
  options.start_id = start_id;
  options.target_id = target_id;
  options.max_depth = max_depth;
  options.direction = traversal_direction(direction);
  options.class_filter = class_filter;
  if (!type_filter.empty()) {
    options.type_filter = type_filter;
  }
  options.limit = limit;
  if (at_us >= 0) options.time_window.at_us = at_us;
  if (start_us >= 0) options.time_window.start_us = start_us;
  if (end_us >= 0) options.time_window.end_us = end_us;
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"SVP/SVPI package inspector"};
  app.set_version_flag("--version",
                       svp::core::tool_version_label("svp-inspector"));
  app.require_subcommand(0, 1);

  std::string package_path;
  bool inspect_json = false;
  auto* inspect =
      app.add_subcommand("inspect", "Print a concise SVP/SVPI package summary");
  inspect
      ->add_option("package", package_path,
                   "Path to an SVP, SVPI, or Embedded SVPI Transport")
      ->required();
  inspect->add_flag("--json", inspect_json,
                    "Emit stable structured JSON output");

  std::string dump_package_path;
  std::string dump_section = "manifest";
  auto* dump =
      app.add_subcommand("dump", "Print manifest or index manifest JSON");
  dump->add_option("package", dump_package_path,
                   "Path to a .svp or .svpi package")
      ->required();
  dump->add_option("--section", dump_section,
                   "Section to dump: manifest, index_manifest, or all")
      ->check(CLI::IsMember({"manifest", "index_manifest", "all"}));

  std::string query_package_path;
  std::string query_mode = "layers";
  std::string query_text;
  std::string query_color_bucket;
  double query_min_coverage = -1.0;
  std::size_t query_limit = 100;
  bool query_json = false;
  auto* query =
      app.add_subcommand("query", "Query SVP/SVPI package semantic layers");
  query->add_option("package", query_package_path,
                    "Path to a .svp or .svpi package")
      ->required();
  query
      ->add_option(
          "--mode", query_mode,
          "Query mode: layers, transcript, words, speakers, ocr, colors, validation, relationships, traverse, path, context, health")
      ->check(CLI::IsMember({"layers", "transcript", "words", "speakers",
                            "ocr", "colors", "validation", "relationships",
                            "traverse", "path", "context", "health"}));
  query->add_option("--text", query_text,
                    "Search text for words or OCR mode");
  query->add_option("--bucket", query_color_bucket,
                    "Filter color observations by dominant bucket");
  query->add_option("--min-coverage", query_min_coverage,
                    "Minimum dominant bucket coverage (0.0-1.0)");
  query->add_option("--limit", query_limit, "Maximum results to return");
  query->add_flag("--json", query_json,
                  "Emit JSON output for agent consumption");

  std::string query_from;
  std::string query_to;
  int query_depth = 2;
  std::string query_direction = "both";
  std::string query_class = "all";
  std::string query_rel_type;
  long long query_at_us = -1;
  long long query_start_us = -1;
  long long query_end_us = -1;
  query->add_option("--from", query_from,
                    "Starting object ID for traverse/path mode");
  query->add_option("--to", query_to, "Target object ID for path mode");
  query->add_option("--depth", query_depth,
                    "Max graph depth for traverse/path mode");
  query
      ->add_option("--direction", query_direction,
                   "Traversal direction: outgoing, incoming, both")
      ->check(CLI::IsMember({"outgoing", "incoming", "both"}));
  query
      ->add_option(
          "--class", query_class,
          "Relationship class filter: support, semantic, unknown, all")
      ->check(CLI::IsMember({"support", "semantic", "unknown", "all"}));
  query->add_option("--type", query_rel_type,
                    "Filter by exact relationship type");
  query->add_option("--at-us", query_at_us,
                    "Filter to relationships active at this microsecond");
  query->add_option("--start-us", query_start_us,
                    "Time window start in microseconds");
  query->add_option("--end-us", query_end_us,
                    "Time window end in microseconds");

  CLI11_PARSE(app, argc, argv);

  if (*inspect) {
    const auto input = embedded_input_controller::preflight(package_path);
    if (input.handled_as_transport) {
      const auto& embedding = input.inspection();
      if (embedding.embeddings.empty()) {
        if (inspect_json) {
          std::cout << nlohmann::json{
              {"embedding",
               embedded_transport_output::embedding_json(embedding)},
          }.dump(2) << "\n";
        } else {
          embedded_transport_output::print_embedding(embedding);
        }
        return embedding.container_structure_valid &&
                       embedding.container.supported
                   ? 0
                   : 1;
      }
      if (!input.semantic_access_allowed) {
        if (inspect_json) {
          std::cout << nlohmann::json{
              {"embedding",
               embedded_transport_output::embedding_json(embedding)},
          }.dump(2) << "\n";
        } else {
          embedded_transport_output::print_embedding(embedding);
        }
        return 1;
      }
      const auto summary = svp::package::read_package_summary(package_path);
      if (inspect_json) {
        std::cout << nlohmann::json{
            {"embedding",
             embedded_transport_output::embedding_json(embedding)},
            {"package", inspect_output::package_summary_json(summary)},
            {"semantic",
             embedded_transport_output::semantic_summary_json(package_path)},
        }.dump(2) << "\n";
      } else {
        embedded_transport_output::print_embedding(embedding);
        std::cout << "\n";
        inspect_output::print_summary(summary);
        embedded_transport_output::print_semantic_summary(package_path);
      }
      return embedding.has_single_valid_embedding() &&
                     embedding.embeddings.front().hash_verified &&
                     embedding.embeddings.front().hash_matches &&
                     summary.layout_readable
                 ? 0
                 : 1;
    }
    const auto summary = svp::package::read_package_summary(package_path);
    if (inspect_json) {
      std::cout << nlohmann::json{
          {"package", inspect_output::package_summary_json(summary)},
          {"semantic",
           embedded_transport_output::semantic_summary_json(package_path)},
      }.dump(2) << "\n";
    } else {
      inspect_output::print_summary(summary);
      embedded_transport_output::print_semantic_summary(package_path);
    }
    return summary.layout_readable ? 0 : 1;
  }

  if (*dump) {
    return dump_command::run(dump_package_path, dump_section);
  }

  if (*query) {
    const auto input =
        embedded_input_controller::preflight(query_package_path);
    if (input.handled_as_transport && !input.semantic_access_allowed) {
      if (query_json) {
        std::cout << nlohmann::json{
            {"embedding", embedded_transport_output::embedding_json(
                              input.inspection())},
            {"error",
             "Embedded SVPI Transport integrity validation failed."},
        }.dump(2) << "\n";
      } else {
        embedded_transport_output::print_embedding(input.inspection());
        std::cerr
            << "Embedded SVPI Transport integrity validation failed.\n";
      }
      return 1;
    }
    if (query_mode == "layers") {
      query_cmd::print_layers(query_package_path, query_json);
    } else if (query_mode == "transcript") {
      query_cmd::print_transcript(query_package_path, query_json);
    } else if (query_mode == "words") {
      query_cmd::print_find_words(query_package_path, query_text, query_limit,
                                  query_json);
    } else if (query_mode == "speakers") {
      query_cmd::print_speakers(query_package_path, query_json);
    } else if (query_mode == "ocr") {
      std::optional<std::string> text_filter;
      if (!query_text.empty()) {
        text_filter = query_text;
      }
      query_cmd::print_ocr(query_package_path, text_filter, query_limit,
                           query_json);
    } else if (query_mode == "colors") {
      std::optional<std::string> dominant_filter;
      if (!query_color_bucket.empty()) {
        dominant_filter = query_color_bucket;
      }
      std::optional<double> min_coverage;
      if (query_min_coverage >= 0.0) {
        min_coverage = query_min_coverage;
      }
      query_cmd::print_colors(query_package_path, dominant_filter,
                              min_coverage, query_limit, query_json);
    } else if (query_mode == "validation") {
      query_cmd::print_validation(query_package_path, query_json);
    } else if (query_mode == "relationships") {
      std::optional<std::string> class_filter;
      if (query_class != "all" && !query_class.empty()) {
        class_filter = query_class;
      }
      query_cmd::print_relationships(query_package_path, class_filter,
                                     query_limit, query_json);
    } else if (query_mode == "traverse") {
      const auto options = make_traversal_options(
          query_from, {}, query_depth, query_direction, query_class,
          query_rel_type, query_limit, query_at_us, query_start_us,
          query_end_us);
      query_cmd::print_traversal(query_package_path, options, query_json);
    } else if (query_mode == "path") {
      const auto options = make_traversal_options(
          query_from, query_to, query_depth, query_direction, query_class,
          query_rel_type, query_limit, query_at_us, query_start_us,
          query_end_us);
      query_cmd::print_path(query_package_path, options, query_json);
    } else if (query_mode == "context") {
      svp::query::ContextOptions options;
      options.object_id = query_from;
      options.max_depth = query_depth;
      options.limit = query_limit;
      options.class_filter = query_class;
      if (query_at_us >= 0) options.at_us = query_at_us;
      if (query_start_us >= 0) options.start_us = query_start_us;
      if (query_end_us >= 0) options.end_us = query_end_us;
      const auto result =
          svp::query::build_context(query_package_path, options);
      if (query_json) {
        std::cout << svp::query::context_result_to_json(result).dump(2)
                  << "\n";
      } else {
        query_cmd::print_context_result(result);
      }
    } else if (query_mode == "health") {
      const auto health =
          svp::query::compute_graph_health(query_package_path);
      if (query_json) {
        std::cout << svp::query::graph_health_to_json(health).dump(2)
                  << "\n";
      } else {
        std::cout << "Graph health diagnostics\n";
        std::cout << "  total edges: " << health.total_edges << "\n";
        std::cout << "  total nodes: " << health.total_nodes << "\n";
        std::cout << "  resolved nodes: " << health.resolved_nodes << "\n";
        std::cout << "  unresolved nodes: " << health.unresolved_nodes
                  << "\n";
        std::cout << "  orphan nodes: " << health.orphan_nodes << "\n";
        std::cout << "  unknown relationship types: "
                  << health.unknown_relationship_types << "\n";
        if (!health.class_counts.empty()) {
          std::cout << "  class counts:\n";
          for (const auto& [relationship_class, count] :
               health.class_counts) {
            std::cout << "    " << relationship_class << ": " << count
                      << "\n";
          }
        }
        if (!health.type_counts.empty()) {
          std::cout << "  type counts:\n";
          for (const auto& [type, count] : health.type_counts) {
            std::cout << "    " << type << ": " << count << "\n";
          }
        }
        if (!health.unresolved_ids.empty()) {
          std::cout << "  unresolved ids: " << health.unresolved_ids.size()
                    << "\n";
          for (const auto& id : health.unresolved_ids) {
            std::cout << "    - " << id << "\n";
          }
        }
        if (!health.orphan_ids.empty()) {
          std::cout << "  orphan ids: " << health.orphan_ids.size() << "\n";
          for (const auto& id : health.orphan_ids) {
            std::cout << "    - " << id << "\n";
          }
        }
        if (!health.error_message.empty()) {
          std::cout << "  error: " << health.error_message << "\n";
        }
      }
    }
    return 0;
  }

  return 0;
}
