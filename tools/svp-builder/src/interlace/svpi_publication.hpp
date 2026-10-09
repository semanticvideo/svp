#pragma once

// Writing an SVPI sidecar from a package build's staging directory: the
// interlace create publication steps, shared by the SVPI write task and
// interlace create's core-only diagnostic.

#include "svp/builder/build_progress.hpp"
#include "svp/builder/interlace.hpp"
#include "svp/package/media_binding.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace svp::builder {

struct SvpiWriteRequest {
  std::string source_path;
  std::string output_path;
};

// Per-section "generated"/"not_generated" from what staging holds.
[[nodiscard]] nlohmann::json detect_section_states(const std::filesystem::path& staging_dir);

// True when any section is "generated".
[[nodiscard]] bool has_semantic_content(const nlohmann::json& sections);

// Records the interlace provenance in staging, rewrites the index with the
// SVPI manifest, writes the SVPI, and validates it.
[[nodiscard]] InterlaceCreateResult write_svpi_from_staging(
    const SvpiWriteRequest& options,
    const svp::package::MediaBindingDocument& binding_doc,
    const std::string& blake3_state,
    const std::filesystem::path& staging_dir,
    const nlohmann::json& sections,
    const std::string& provenance_notes,
    BuildProgressSink& sink);

// Replaces staging with core-only content and writes an SVPI whose sections
// are all `section_state`.
[[nodiscard]] InterlaceCreateResult write_core_only_svpi(
    const SvpiWriteRequest& options,
    const svp::package::MediaBindingDocument& binding_doc,
    const std::string& blake3_state,
    const std::string& section_state,
    const std::string& notes,
    const std::filesystem::path& staging_dir,
    BuildProgressSink& sink);

// Provenance notes of the two staging-based outcomes.
[[nodiscard]] std::string svpi_provenance_notes(bool semantic_content);
inline constexpr const char* kSvpiBlockedNotes =
    "SVPI sidecar created with core-only content (semantic pipeline failed, sections marked blocked)";

}  // namespace svp::builder
