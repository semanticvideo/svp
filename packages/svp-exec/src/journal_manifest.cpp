#include "journal_manifest.hpp"

#include "durable_io.hpp"
#include "journal_schema.hpp"
#include "svp/exec/canonical_json.hpp"
#include "svp/exec/exec_error.hpp"
#include "svp/exec/journal_error.hpp"
#include "verified_blob.hpp"

#include <fstream>
#include <iterator>

namespace svp::exec::detail {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kDatabaseFileName = "build.sqlite";

[[noreturn]] void incompatible(const JournalLayout& layout, std::string_view why) {
  throw JournalError(JournalErrorCode::incompatible,
                     layout.manifest.string() + ": " + std::string(why));
}

}  // namespace

void write_journal_manifest(const JournalLayout& layout, const JournalManifest& manifest) {
  const std::string text = encode_canonical_json(nlohmann::json{
      {"build_session_id", manifest.build_session_id},
      {"created_utc", manifest.created_utc},
      {"database", kDatabaseFileName},
      {"format", kJournalManifestFormat},
      {"journal_schema_version", manifest.journal_schema_version},
      {"output_path", manifest.output_path},
  });
  const fs::path staged = layout.pending / pending_file_name();
  std::error_code error =
      write_new_file_synced(staged, std::as_bytes(std::span(text.data(), text.size())));
  if (!error) {
    fs::rename(staged, layout.manifest, error);
  }
  if (!error) {
    error = sync_directory(layout.root);
  }
  if (error) {
    std::error_code ignored;
    fs::remove(staged, ignored);
    throw JournalError(JournalErrorCode::io_error,
                       "write " + layout.manifest.string() + ": " + error.message());
  }
}

JournalManifest read_journal_manifest(const JournalLayout& layout) {
  std::ifstream input(layout.manifest, std::ios::binary);
  if (!input) {
    incompatible(layout, "missing or unreadable");
  }
  const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  nlohmann::json value;
  try {
    value = decode_canonical_json(text);
  } catch (const ExecError& error) {
    incompatible(layout, error.what());
  }
  if (!value.is_object() || value.value("format", "") != kJournalManifestFormat) {
    incompatible(layout, "not an svp-recovery-journal-v1 manifest");
  }
  JournalManifest manifest;
  try {
    manifest.build_session_id = value.at("build_session_id").get<std::string>();
    manifest.created_utc = value.at("created_utc").get<std::string>();
    manifest.output_path = value.at("output_path").get<std::string>();
    manifest.journal_schema_version = value.at("journal_schema_version").get<std::int64_t>();
  } catch (const nlohmann::json::exception& error) {
    incompatible(layout, error.what());
  }
  if (manifest.journal_schema_version != kJournalSchemaVersion) {
    incompatible(layout, "journal schema version " +
                             std::to_string(manifest.journal_schema_version) + ", expected " +
                             std::to_string(kJournalSchemaVersion));
  }
  return manifest;
}

}  // namespace svp::exec::detail
