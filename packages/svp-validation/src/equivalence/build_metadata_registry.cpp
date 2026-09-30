#include "equivalence/build_metadata_registry.hpp"

#include <array>

namespace svp::validation::equivalence {
namespace {

// Single owner of the build-metadata normalization list. Add a field here
// only when its value is wall-clock time or a build-host location by
// construction, and state the writer that makes it so.
constexpr std::array kJsonBuildMetadataFields{
    JsonBuildMetadataField{
        .entry = "manifest.json",
        .pointer = "/created_utc",
        .reason = "Wall-clock time at package assembly (svp-builder "
                  "make_package_manifest; SVPI interlace create/extract).",
    },
    JsonBuildMetadataField{
        .entry = "manifest.json",
        .pointer = "/extraction_source/extracted_utc",
        .reason = "Wall-clock time of SVPI extraction (svp-builder interlace extract).",
    },
    JsonBuildMetadataField{
        .entry = "provenance/processors.jsonl",
        .pointer = "/ran_utc",
        .reason = "Wall-clock time a processor ran (svp-builder interlace stages).",
    },
    JsonBuildMetadataField{
        .entry = "provenance/interlace_events.jsonl",
        .pointer = "/event_utc",
        .reason = "Wall-clock time of an interlace event (svp-builder interlace stages).",
    },
    JsonBuildMetadataField{
        .entry = "provenance/processors.jsonl",
        .pointer = "/input_refs/*",
        .filter = BuildMetadataValueFilter::absolute_host_path,
        .reason = "Absolute build-host path of the source media recorded by the "
                  "FFmpeg audio extraction processor; package-relative refs stay exact.",
    },
    JsonBuildMetadataField{
        .entry = "media_binding.json",
        .pointer = "/bindings/*/location_hints/original_absolute_path",
        .reason = "Absolute build-host path of the bound media (location hint, "
                  "not media identity).",
    },
    JsonBuildMetadataField{
        .entry = "media_binding.json",
        .pointer = "/bindings/*/location_hints/volume_hint",
        .reason = "Build-host volume name of the bound media (location hint, "
                  "not media identity).",
    },
    JsonBuildMetadataField{
        .entry = "media_binding.json",
        .pointer = "/bindings/*/location_hints/last_seen_utc",
        .reason = "Wall-clock time the bound media was last seen (location hint).",
    },
};

constexpr std::array kSqliteBuildMetadataRows{
    SqliteBuildMetadataRow{
        .table = "svp_meta",
        .key_column = "key",
        .key_text = "created_utc",
        .value_column = "value",
        .reason = "Copy of manifest.json created_utc written by the index writer.",
    },
};

}  // namespace

std::span<const JsonBuildMetadataField> json_build_metadata_fields() {
  return kJsonBuildMetadataFields;
}

std::span<const SqliteBuildMetadataRow> sqlite_build_metadata_rows() {
  return kSqliteBuildMetadataRows;
}

}  // namespace svp::validation::equivalence
