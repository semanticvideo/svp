#pragma once

#include "svp/package/embedded_svpi.hpp"
#include "svp/package/package_summary.hpp"

#include <nlohmann/json.hpp>
#include <filesystem>

namespace embedded_transport_output {

[[nodiscard]] nlohmann::json embedding_json(
    const svp::package::EmbeddedSvpiInspection& inspection);
[[nodiscard]] nlohmann::json package_summary_json(
    const svp::package::PackageSummary& summary);
void print_embedding(const svp::package::EmbeddedSvpiInspection& inspection);
[[nodiscard]] nlohmann::json semantic_summary_json(
    const std::filesystem::path& package_path);
void print_semantic_summary(const std::filesystem::path& package_path);

}  // namespace embedded_transport_output
