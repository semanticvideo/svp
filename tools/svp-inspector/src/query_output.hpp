#pragma once

#include "svp/query/traversal.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace query_cmd {

void print_layers(const std::filesystem::path& package_path, bool json_output);
void print_transcript(const std::filesystem::path& package_path,
                      bool json_output);
void print_find_words(const std::filesystem::path& package_path,
                      const std::string& search_text,
                      std::size_t max_results,
                      bool json_output);
void print_speakers(const std::filesystem::path& package_path,
                    bool json_output);
void print_ocr(const std::filesystem::path& package_path,
               const std::optional<std::string>& text_filter,
               std::size_t max_results,
               bool json_output);
void print_colors(const std::filesystem::path& package_path,
                  const std::optional<std::string>& dominant_filter,
                  std::optional<double> min_coverage,
                  std::size_t max_results,
                  bool json_output);
void print_validation(const std::filesystem::path& package_path,
                      bool json_output);
void print_relationships(
    const std::filesystem::path& package_path,
    const std::optional<std::string>& class_filter,
    std::size_t max_results,
    bool json_output);
void print_traversal(const std::filesystem::path& package_path,
                     const svp::query::TraversalOptions& options,
                     bool json_output);
void print_path(const std::filesystem::path& package_path,
                const svp::query::TraversalOptions& options,
                bool json_output);
void print_context_result(const svp::query::ContextResult& result);

}  // namespace query_cmd
