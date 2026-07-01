#include "svp/builder/interlace_batch.hpp"
#include "interlace_batch_internal.hpp"

#include "svp/package/media_binding.hpp"
#include "svp/package/media_binding_factory.hpp"
#include "svp/package/package_layout.hpp"
#include "svp/validation/svpi_validator.hpp"
#include "svp/validation/report_json.hpp"

#include <filesystem>
#include <set>
#include <string>

namespace svp::builder {

ScanResult interlace_scan(const ScanOptions& options) {
  ScanResult result;

  const std::filesystem::path source_dir(options.source_dir);
  if (!std::filesystem::exists(source_dir) || !std::filesystem::is_directory(source_dir)) {
    return result;
  }

  auto media_files = discover_media_files(source_dir, options.recursive);
  auto svpi_files = discover_svpi_files(source_dir, options.recursive);

  result.total_media = static_cast<int>(media_files.size());
  result.total_svpi = static_cast<int>(svpi_files.size());

  std::set<std::filesystem::path> matched_svpi;

  for (const auto& media_path : media_files) {
    ScanPairInfo pair;
    pair.media_path = media_path;
    pair.media_filename = media_path.filename().string();
    pair.media_relative_path =
        std::filesystem::relative(media_path, source_dir).string();

    auto candidate = find_candidate_sidecar(media_path, media_path.parent_path());
    if (!candidate.empty()) {
      pair.svpi_path = candidate;
      pair.svpi_found = true;
      matched_svpi.insert(candidate);

      svp::validation::SvpiValidatorOptions vopts;
      vopts.validation_codes_path = options.validation_codes_path;
      auto report = svp::validation::validate_svpi_package(candidate, vopts);

      if (svp::validation::exit_code(report) == 0) {
        auto binding_entry = svp::package::read_package_entry(candidate, "media_binding.json");
        if (binding_entry.has_value()) {
          auto binding_doc = svp::package::parse_media_binding_json(binding_entry.value());
          auto verification = svp::package::verify_media_binding(media_path, binding_doc);
          pair.binding_state_label = verification.state_label;
          pair.binding_verified =
              (verification.state == svp::package::BindingVerificationState::verified);
          if (pair.binding_verified) {
            result.verified_pairs++;
          }
        }
      } else {
        pair.svpi_error = "structure validation failed";
      }

      result.matched_pairs++;
    } else {
      result.missing_sidecars.push_back(pair.media_relative_path);
    }

    result.pairs.push_back(std::move(pair));
  }

  for (const auto& svpi_path : svpi_files) {
    if (matched_svpi.find(svpi_path) == matched_svpi.end()) {
      auto rel = std::filesystem::relative(svpi_path, source_dir).string();
      result.unbound_sidecars.push_back(rel);
    }
  }

  return result;
}

}  // namespace svp::builder
