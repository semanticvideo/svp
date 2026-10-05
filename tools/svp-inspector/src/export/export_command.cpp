#include "export_command.hpp"

#include "block_stream_export.hpp"
#include "export_error.hpp"
#include "export_plan.hpp"
#include "export_summary.hpp"
#include "input_identity.hpp"
#include "jsonl_line_reader.hpp"
#include "layer_result.hpp"
#include "layer_writers.hpp"
#include "output_transaction.hpp"
#include "package_kind.hpp"
#include "package_reader.hpp"
#include "reference_resolver.hpp"
#include "validation_gate.hpp"
#include "version_policy.hpp"

#include "svp/package/embedded_package_session.hpp"
#include "svp/validation/report_json.hpp"

#include <nlohmann/json.hpp>

#include <exception>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace package_export {
namespace {

constexpr std::string_view kManifestEntry = "manifest.json";
constexpr std::string_view kMediaBindingEntry = "media_binding.json";
constexpr std::string_view kMimetypeEntry = "mimetype";

struct ExportOutcome {
  PackageKind kind = PackageKind::svp;
  std::size_t layer_count = 0;
  std::size_t file_count = 0;
};

// The input after validation: an open reader on a package that passed
// validation and the version policy.
struct AcceptedPackage {
  PackageKind kind = PackageKind::svp;
  InputIdentity identity;
  std::unique_ptr<svp::package::VerifiedEmbeddedPackageSession> session;
  std::unique_ptr<PackageReader> reader;
  svp::validation::ValidationReport report;
  DeclaredVersions versions;
  nlohmann::json manifest;
};

InputIdentity require_readable_input(const std::filesystem::path& path) {
  const auto identity = read_input_identity(path);
  if (!identity.has_value()) {
    throw ExportError(ExportErrorCode::input_missing,
                      "Input file does not exist or cannot be examined.");
  }
  if (!identity->is_regular_file) {
    throw ExportError(ExportErrorCode::input_not_regular_file,
                      "Input path is not a regular file.");
  }
  return *identity;
}

nlohmann::json report_details(const svp::validation::ValidationReport& report) {
  nlohmann::json validation = report;
  return nlohmann::json{{"validation", std::move(validation)}};
}

nlohmann::json version_details(const VersionProblem& problem) {
  return nlohmann::json{{"field", problem.field},
                        {"declared", problem.declared},
                        {"supported", problem.supported}};
}

// Parsed JSON document, or null when the entry is absent or not JSON.
nlohmann::json read_json_entry(const PackageReader& reader,
                               std::string_view name) {
  const auto* entry = reader.find_file(name);
  if (entry == nullptr) {
    return nullptr;
  }
  auto document = nlohmann::json::parse(
      reader.read_whole(*entry, kMaxRecordBytes), nullptr, false);
  return document.is_discarded() ? nlohmann::json(nullptr) : document;
}

std::optional<PackageByteRange> embedded_range(
    const svp::package::VerifiedEmbeddedPackageSession* session) {
  if (session == nullptr || !session->valid()) {
    return std::nullopt;
  }
  return PackageByteRange{session->payload_offset(), session->payload_size()};
}

// Validation first (Section 1.2), then the version policy (Section 1.3).
AcceptedPackage accept_package(const std::filesystem::path& path) {
  AcceptedPackage package;
  package.identity = require_readable_input(path);
  package.kind = detect_package_kind(path);
  if (package.kind == PackageKind::embedded_svpi) {
    // Verifies the transport once and lets every later read of the embedded
    // package reuse that verification.
    package.session =
        std::make_unique<svp::package::VerifiedEmbeddedPackageSession>(path);
  }

  package.report = validate_for_export(path, package.kind);
  const auto& report = package.report;
  if (validation_resources_unavailable(report)) {
    throw ExportError(ExportErrorCode::validation_unavailable,
                      "The validator's registries and schemas could not be "
                      "loaded, so the package could not be validated.",
                      report_details(report));
  }
  std::optional<std::uint16_t> transport_profile;
  if (report.embedding_transport.embedding_detected) {
    transport_profile = report.embedding_transport.profile_version;
  }
  if (embedded_profile_unsupported(report)) {
    VersionProblem problem{
        .field = "embedded_transport_profile_version",
        .declared = transport_profile ? nlohmann::json(*transport_profile)
                                      : nlohmann::json(nullptr),
        .supported = supported_transport_profile_versions(),
    };
    throw ExportError(ExportErrorCode::unsupported_version,
                      "The Embedded SVPI Transport profile version is not "
                      "supported by this build.",
                      version_details(problem));
  }

  const bool can_open = package.kind != PackageKind::embedded_svpi ||
                        embedded_range(package.session.get()).has_value();
  if (can_open) {
    try {
      package.reader = std::make_unique<PackageReader>(
          path, embedded_range(package.session.get()));
      package.manifest = read_json_entry(*package.reader, kManifestEntry);
    } catch (const ExportError&) {
      // An unreadable package is reported by its validation result below.
      if (validation_passed(report)) {
        throw;
      }
    }
  }

  package.versions =
      declared_versions(package.manifest, package.kind, transport_profile);
  if (const auto problem =
          find_unsupported_declared_version(package.versions, package.kind)) {
    throw ExportError(ExportErrorCode::unsupported_version, problem->message,
                      version_details(*problem));
  }
  if (!validation_passed(report)) {
    throw ExportError(ExportErrorCode::package_invalid,
                      "Package failed validation; nothing was exported.",
                      report_details(report));
  }
  if (!package.reader) {
    throw ExportError(ExportErrorCode::entry_unreadable,
                      "Package could not be opened after validation.");
  }
  if (!package.manifest.is_object()) {
    throw ExportError(ExportErrorCode::malformed_document,
                      "manifest.json is missing or is not a JSON object.",
                      entry_details(kManifestEntry));
  }
  if (const auto problem =
          find_missing_required_version(package.versions, package.kind)) {
    throw ExportError(ExportErrorCode::unsupported_version, problem->message,
                      version_details(*problem));
  }
  return package;
}

nlohmann::json mimetype_json(const PackageReader& reader) {
  const auto* entry = reader.find_file(kMimetypeEntry);
  if (entry == nullptr) {
    return nullptr;
  }
  nlohmann::json value = reader.read_whole(*entry, kMaxRecordBytes);
  try {
    (void)value.dump();  // Rejects bytes that are not UTF-8 text.
  } catch (const nlohmann::json::exception&) {
    return nullptr;
  }
  return value;
}

nlohmann::json media_binding_json(const PackageReader& reader) {
  if (reader.find_file(kMediaBindingEntry) == nullptr) {
    return nullptr;
  }
  auto binding = read_json_entry(reader, kMediaBindingEntry);
  if (binding.is_null()) {
    throw ExportError(ExportErrorCode::malformed_document,
                      "A JSON package entry is not valid JSON.",
                      entry_details(kMediaBindingEntry));
  }
  return binding;
}

nlohmann::json embedded_transport_json(
    const svp::validation::ValidationReport& report) {
  const auto& transport = report.embedding_transport;
  if (!transport.present) {
    return nullptr;
  }
  return {
      {"container_kind", transport.container_kind},
      {"major_brand", transport.major_brand},
      {"profile_version", transport.profile_version},
      {"box_offset", transport.box_offset},
      {"box_size", transport.box_size},
      {"payload_offset", transport.payload_offset},
      {"payload_size", transport.payload_size},
  };
}

std::vector<LayerResult> write_layers(const PackageReader& reader,
                                      const ExportPlan& plan,
                                      ReferenceResolver& resolver,
                                      OutputTransaction& output) {
  std::vector<LayerResult> results(plan.layers.size());
  for (std::size_t index = 0; index < plan.layers.size(); ++index) {
    results[index].plan = &plan.layers[index];
  }
  // Block streams first: index records resolve against their decoded blocks.
  for (auto& result : results) {
    if (result.plan->representation == Representation::block_stream) {
      resolver.add_block_stream(
          export_block_stream(reader, *result.plan, output, result));
    }
  }
  resolver.load_lookups(reader);
  for (auto& result : results) {
    switch (result.plan->representation) {
      case Representation::jsonl:
        export_jsonl_layer(reader, *result.plan, resolver, output, result);
        break;
      case Representation::json:
        export_json_document(reader, *result.plan, output, result);
        break;
      case Representation::file:
        export_file_bytes(reader, *result.plan, output, result);
        break;
      case Representation::block_stream:
        break;
    }
  }
  return results;
}

void require_unchanged_input(const std::filesystem::path& path,
                             const AcceptedPackage& package) {
  const auto identity = read_input_identity(path);
  const bool session_matches =
      package.session == nullptr || package.session->matches(path);
  if (!identity.has_value() || *identity != package.identity ||
      !session_matches) {
    throw ExportError(ExportErrorCode::package_changed,
                      "The input file changed while it was being exported.");
  }
}

ExportOutcome export_package(const ExportOptions& options) {
  const auto package = accept_package(options.package);
  const auto& reader = *package.reader;
  const auto plan = make_export_plan(reader.entries());

  OutputTransaction::check_target(options.out, options.overwrite);
  OutputTransaction::check_input_outside(options.package, options.out);
  OutputTransaction::check_free_space(options.out, plan.declared_input_bytes);
  OutputTransaction output{options.out, options.overwrite};

  ReferenceResolver resolver{plan};
  const auto layers = write_layers(reader, plan, resolver, output);

  const auto summary = build_export_summary(SummaryInputs{
      .kind = package.kind,
      .mimetype = mimetype_json(reader),
      .versions = package.versions,
      .embedded_transport = embedded_transport_json(package.report),
      .validation = validation_summary_json(package.report),
      .manifest = package.manifest,
      .media_binding = media_binding_json(reader),
      .resolution_sources = resolver.sources_json(),
      .layers = &layers,
  });
  auto summary_file = output.create_file(kSummaryFileName);
  summary_file.write(summary.dump(2));
  summary_file.write("\n");
  summary_file.close();

  require_unchanged_input(options.package, package);
  output.publish();

  ExportOutcome outcome;
  outcome.kind = package.kind;
  outcome.layer_count = layers.size();
  outcome.file_count = 1;  // export.json
  for (const auto& layer : plan.layers) {
    outcome.file_count += layer.files.size();
  }
  return outcome;
}

void print_result(std::ostream& stream, const nlohmann::json& result) {
  // Replacement keeps the result printable when a path or message holds bytes
  // that are not UTF-8.
  stream << result.dump(2, ' ', false, nlohmann::json::error_handler_t::replace)
         << "\n";
}

nlohmann::json error_result(ExportErrorCode code, const std::string& message,
                            const nlohmann::json& details) {
  return {
      {"status", "error"},
      {"exit_code", exit_code_for(code)},
      {"error",
       {{"code", to_string(code)},
        {"message", message},
        {"details", details}}},
  };
}

}  // namespace

int run_export(const ExportOptions& options, std::ostream& result_stream) {
  try {
    const auto outcome = export_package(options);
    print_result(result_stream,
                 {
                     {"status", "exported"},
                     {"exit_code", kExitExported},
                     {"out", options.out.string()},
                     {"package_kind", to_string(outcome.kind)},
                     {"export_format_version", kExportFormatVersion},
                     {"layer_count", outcome.layer_count},
                     {"file_count", outcome.file_count},
                 });
    return kExitExported;
  } catch (const ExportError& error) {
    print_result(result_stream,
                 error_result(error.code(), error.what(), error.details()));
    return exit_code_for(error.code());
  } catch (const std::exception& error) {
    print_result(result_stream,
                 error_result(ExportErrorCode::internal_error, error.what(),
                              nlohmann::json::object()));
    return exit_code_for(ExportErrorCode::internal_error);
  }
}

}  // namespace package_export
