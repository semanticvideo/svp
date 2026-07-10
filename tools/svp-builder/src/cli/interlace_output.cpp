#include "cli_context.hpp"

#include "svp/builder/interlace.hpp"
#include "svp/builder/interlace_batch.hpp"
#include "svp/validation/report_json.hpp"

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>

namespace {

void render_validate_json(const svp::builder::InterlaceValidateResult& result,
                          const std::string& svpi_path) {
  nlohmann::json j;
  j["structure_valid"] = result.structure_valid;
  j["binding_attempted"] = result.binding_attempted;
  j["binding_verified"] = result.binding_verified;
  j["binding_state"] = result.binding_state_label;
  j["binding_passing_checks"] = result.binding_passing_checks;
  j["binding_failing_checks"] = result.binding_failing_checks;
  nlohmann::json report_json;
  svp::validation::to_json(report_json, result.validation_report);
  j["validation_report"] = report_json;
  std::cout << j.dump(2) << "\n";
}

void render_validate_plain(const svp::builder::InterlaceValidateResult& result,
                           const std::string& svpi_path) {
  std::cout << "SVPI: " << svpi_path << "\n";
  std::cout << "Structure valid: " << (result.structure_valid ? "yes" : "no") << "\n";
  if (result.binding_attempted) {
    std::cout << "Binding state: " << result.binding_state_label << "\n";
    if (!result.binding_passing_checks.empty()) {
      std::cout << "Passing checks:\n";
      for (const auto& check : result.binding_passing_checks) {
        std::cout << "  + " << check << "\n";
      }
    }
    if (!result.binding_failing_checks.empty()) {
      std::cout << "Failing checks:\n";
      for (const auto& check : result.binding_failing_checks) {
        std::cout << "  - " << check << "\n";
      }
    }
  } else {
    std::cout << "Binding: not checked (no --media supplied)\n";
  }
  if (!result.validation_report.errors.empty()) {
    std::cout << "Validation errors:\n";
    for (const auto& err : result.validation_report.errors) {
      std::cout << "  " << err.code << ": " << err.message << "\n";
    }
  }
}

void render_inspect_json(const svp::builder::InterlaceInspectResult& result) {
  nlohmann::json j;
  j["artifact_type"] = result.artifact_type;
  j["svpi_version"] = result.svpi_version;
  j["manifest_format"] = result.manifest_format;
  j["entry_count"] = result.entry_count;
  j["root_entries"] = result.root_entries;
  j["binding_id"] = result.binding_id;
  j["binding_contract"] = result.binding_contract;
  j["binding_verification_state"] = result.binding_verification_state;
  j["blake3_state"] = result.blake3_state;
  j["blake3_hash"] = result.blake3_hash;
  j["media_id"] = result.media_id;
  j["size_bytes"] = result.size_bytes;
  j["duration_us"] = result.duration_us;
  j["container_format"] = result.container_format;
  j["original_filename_hint"] = result.original_filename_hint;
  j["has_media_original"] = result.has_media_original;
  j["has_index_sqlite"] = result.has_index_sqlite;
  j["has_index_manifest"] = result.has_index_manifest;
  j["has_provenance"] = result.has_provenance;
  j["recombination_ready"] = result.recombination_ready;
  j["section_states"] = result.section_states;
  std::cout << j.dump(2) << "\n";
}

void render_inspect_plain(const svp::builder::InterlaceInspectResult& result) {
  std::cout << "Artifact type: " << result.artifact_type << "\n";
  std::cout << "SVPI version: " << result.svpi_version << "\n";
  std::cout << "Manifest format: " << result.manifest_format << "\n";
  std::cout << "Entry count: " << result.entry_count << "\n";
  std::cout << "Binding ID: " << result.binding_id << "\n";
  std::cout << "Binding contract: " << result.binding_contract << "\n";
  std::cout << "Binding verification state: " << result.binding_verification_state << "\n";
  std::cout << "BLAKE3 state: " << result.blake3_state << "\n";
  if (!result.blake3_hash.empty()) {
    std::cout << "BLAKE3 hash: " << result.blake3_hash << "\n";
  }
  std::cout << "Media ID: " << result.media_id << "\n";
  std::cout << "Size bytes: " << result.size_bytes << "\n";
  std::cout << "Duration us: " << result.duration_us << "\n";
  std::cout << "Container format: " << result.container_format << "\n";
  std::cout << "Original filename hint: " << result.original_filename_hint << "\n";
  std::cout << "Has media/original: " << (result.has_media_original ? "yes" : "no") << "\n";
  std::cout << "Has index.sqlite: " << (result.has_index_sqlite ? "yes" : "no") << "\n";
  std::cout << "Has index_manifest.json: " << (result.has_index_manifest ? "yes" : "no") << "\n";
  std::cout << "Has provenance: " << (result.has_provenance ? "yes" : "no") << "\n";
  std::cout << "Recombination ready: " << (result.recombination_ready ? "yes" : "no") << "\n";
}

void render_create_batch_json(const svp::builder::BatchCreateResult& result) {
  nlohmann::json j;
  j["created"] = result.created_count;
  j["already_valid"] = result.already_valid_count;
  j["skipped"] = result.skipped_count;
  j["binding_mismatch"] = result.mismatch_count;
  j["failed"] = result.failed_count;
  j["replaced"] = result.replaced_count;
  nlohmann::json files = nlohmann::json::array();
  for (const auto& r : result.results) {
    nlohmann::json file;
    file["source"] = r.source_filename;
    file["artifact"] = r.artifact_path.string();
    file["status"] = std::string(svp::builder::batch_file_status_label(r.status));
    if (!r.error_message.empty()) file["error"] = r.error_message;
    if (!r.blake3_state.empty()) file["blake3_state"] = r.blake3_state;
    files.push_back(std::move(file));
  }
  j["files"] = files;
  std::cout << j.dump(2) << "\n";
}

void render_create_batch_plain(const svp::builder::BatchCreateResult& result) {
  std::cout << "Batch create summary:\n";
  std::cout << "  created: " << result.created_count << "\n";
  std::cout << "  already_valid: " << result.already_valid_count << "\n";
  std::cout << "  binding_mismatch: " << result.mismatch_count << "\n";
  std::cout << "  failed: " << result.failed_count << "\n";
  std::cout << "  replaced: " << result.replaced_count << "\n";
  for (const auto& r : result.results) {
    std::cout << "  " << r.source_filename << ": "
              << svp::builder::batch_file_status_label(r.status) << "\n";
    if (!r.artifact_path.empty()) {
      std::cout << "    artifact: " << r.artifact_path.string() << "\n";
    }
    if (!r.error_message.empty()) {
      std::cout << "    error: " << r.error_message << "\n";
    }
  }
}

void render_scan_json(const svp::builder::ScanResult& result) {
  nlohmann::json j;
  j["total_media"] = result.total_media;
  j["total_svpi"] = result.total_svpi;
  j["matched_pairs"] = result.matched_pairs;
  j["verified_pairs"] = result.verified_pairs;
  j["missing_sidecars"] = result.missing_sidecars;
  j["unbound_sidecars"] = result.unbound_sidecars;
  nlohmann::json pairs = nlohmann::json::array();
  for (const auto& p : result.pairs) {
    nlohmann::json pair;
    pair["media"] = p.media_filename;
    pair["svpi_found"] = p.svpi_found;
    pair["binding_verified"] = p.binding_verified;
    if (!p.binding_state_label.empty()) pair["binding_state"] = p.binding_state_label;
    if (!p.svpi_error.empty()) pair["svpi_error"] = p.svpi_error;
    pairs.push_back(std::move(pair));
  }
  j["pairs"] = pairs;
  std::cout << j.dump(2) << "\n";
}

void render_scan_plain(const svp::builder::ScanResult& result) {
  std::cout << "Scan results:\n";
  std::cout << "  total media: " << result.total_media << "\n";
  std::cout << "  total SVPI: " << result.total_svpi << "\n";
  std::cout << "  matched pairs: " << result.matched_pairs << "\n";
  std::cout << "  verified pairs: " << result.verified_pairs << "\n";
  if (!result.missing_sidecars.empty()) {
    std::cout << "  missing sidecars:\n";
    for (const auto& m : result.missing_sidecars) {
      std::cout << "    " << m << "\n";
    }
  }
  if (!result.unbound_sidecars.empty()) {
    std::cout << "  unbound sidecars:\n";
    for (const auto& u : result.unbound_sidecars) {
      std::cout << "    " << u << "\n";
    }
  }
  for (const auto& p : result.pairs) {
    std::cout << "  " << p.media_filename << " -> "
              << (p.svpi_found ? "paired" : "no sidecar")
              << (p.binding_verified ? " (verified)" : "")
              << "\n";
  }
}

void render_validate_batch_json(const svp::builder::BatchValidateResult& result) {
  nlohmann::json j;
  j["valid_bound"] = result.valid_bound_count;
  j["valid_unbound"] = result.valid_unbound_count;
  j["binding_mismatch"] = result.mismatch_count;
  j["invalid_structure"] = result.invalid_structure_count;
  j["failed"] = result.failed_count;
  nlohmann::json files = nlohmann::json::array();
  for (const auto& r : result.results) {
    nlohmann::json file;
    file["svpi"] = r.svpi_filename;
    file["state"] = std::string(svp::builder::batch_validation_state_label(r.state));
    if (!r.media_filename.empty()) file["media"] = r.media_filename;
    if (!r.errors.empty()) file["errors"] = r.errors;
    files.push_back(std::move(file));
  }
  j["files"] = files;
  std::cout << j.dump(2) << "\n";
}

void render_validate_batch_plain(const svp::builder::BatchValidateResult& result) {
  std::cout << "Batch validate summary:\n";
  std::cout << "  valid_bound: " << result.valid_bound_count << "\n";
  std::cout << "  valid_unbound: " << result.valid_unbound_count << "\n";
  std::cout << "  binding_mismatch: " << result.mismatch_count << "\n";
  std::cout << "  invalid_structure: " << result.invalid_structure_count << "\n";
  std::cout << "  failed: " << result.failed_count << "\n";
  for (const auto& r : result.results) {
    std::cout << "  " << r.svpi_filename << ": "
              << svp::builder::batch_validation_state_label(r.state) << "\n";
    for (const auto& err : r.errors) {
      std::cout << "    " << err << "\n";
    }
  }
}

void render_complete_identity_batch_json(
    const svp::builder::CompleteIdentityBatchResult& result) {
  nlohmann::json j;
  j["completed"] = result.completed_count;
  j["already_present"] = result.already_present_count;
  j["failed"] = result.failed_count;
  std::cout << j.dump(2) << "\n";
}

void render_complete_identity_batch_plain(
    const svp::builder::CompleteIdentityBatchResult& result) {
  std::cout << "Complete-identity batch summary:\n";
  std::cout << "  completed: " << result.completed_count << "\n";
  std::cout << "  already_present: " << result.already_present_count << "\n";
  std::cout << "  failed: " << result.failed_count << "\n";
  for (size_t i = 0; i < result.results.size(); ++i) {
    std::cout << "  " << result.svpi_filenames[i] << ": "
              << (result.results[i].success ? "ok" : "failed");
    if (!result.results[i].error_message.empty()) {
      std::cout << " - " << result.results[i].error_message;
    }
    std::cout << "\n";
  }
}

}  // namespace

void render_validate_output(const svp::builder::InterlaceValidateResult& result,
                            const std::string& svpi_path, bool json) {
  if (json) {
    render_validate_json(result, svpi_path);
  } else {
    render_validate_plain(result, svpi_path);
  }
}

void render_inspect_output(const svp::builder::InterlaceInspectResult& result,
                           bool json) {
  if (json) {
    render_inspect_json(result);
  } else {
    render_inspect_plain(result);
  }
}

void render_create_batch_output(const svp::builder::BatchCreateResult& result,
                                bool json) {
  if (json) {
    render_create_batch_json(result);
  } else {
    render_create_batch_plain(result);
  }
}

void render_scan_output(const svp::builder::ScanResult& result, bool json) {
  if (json) {
    render_scan_json(result);
  } else {
    render_scan_plain(result);
  }
}

void render_validate_batch_output(const svp::builder::BatchValidateResult& result,
                                  bool json) {
  if (json) {
    render_validate_batch_json(result);
  } else {
    render_validate_batch_plain(result);
  }
}

void render_complete_identity_batch_output(
    const svp::builder::CompleteIdentityBatchResult& result, bool json) {
  if (json) {
    render_complete_identity_batch_json(result);
  } else {
    render_complete_identity_batch_plain(result);
  }
}
