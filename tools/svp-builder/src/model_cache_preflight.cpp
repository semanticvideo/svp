#include "svp/builder/model_cache_preflight.hpp"

#include "svp/models/model_lock.hpp"
#include "svp/models/verification.hpp"

#include <exception>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace svp::builder {
namespace {

[[noreturn]] void throw_preflight_failure(
    const std::filesystem::path& model_cache_dir,
    const std::vector<std::string>& errors) {
  std::ostringstream message;
  message << "authoritative model-cache verification failed for "
          << model_cache_dir.string();
  for (const std::string& error : errors) {
    message << "\n- " << error;
  }
  throw ModelCachePreflightError(message.str());
}

}  // namespace

bool schedules_model_backed_work(
    const BuildStageExecutionPlan& stage_plan) noexcept {
  return stage_plan.run_audio || stage_plan.run_foundation_ocr ||
         stage_plan.run_package_skeleton;
}

void verify_authoritative_model_cache(
    const std::filesystem::path& model_cache_dir) {
  const std::filesystem::path lock_path = model_cache_dir / "model-lock.json";
  std::error_code error;
  if (!std::filesystem::is_regular_file(lock_path, error)) {
    throw_preflight_failure(
        model_cache_dir,
        {"required root model lock is missing: " + lock_path.string()});
  }

  svp::models::ModelLock lock;
  try {
    lock = svp::models::load_model_lock(lock_path);
  } catch (const std::exception& error) {
    throw_preflight_failure(model_cache_dir, {error.what()});
  }

  const svp::models::VerificationReport report =
      svp::models::verify_lock_against_cache(lock, model_cache_dir);
  if (report.ok()) {
    return;
  }

  std::vector<std::string> errors;
  for (const svp::models::VerificationIssue& issue : report.issues) {
    if (issue.severity == svp::models::VerificationSeverity::error) {
      errors.push_back(issue.message);
    }
  }
  throw_preflight_failure(model_cache_dir, errors);
}

}  // namespace svp::builder
