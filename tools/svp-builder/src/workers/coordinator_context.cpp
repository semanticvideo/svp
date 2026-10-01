#include "coordinator_context.hpp"

#include "svp/exec/worker/coordinator_session.hpp"
#include "svp/exec/worker/model_bundles.hpp"
#include "svp/exec/worker/worker_error.hpp"
#include "svp/models/cache.hpp"

#include <mach-o/dyld.h>

#include <cstdint>
#include <vector>

namespace svp::builder::workers {

using svp::exec::worker::WorkerError;
using svp::exec::worker::WorkerErrorCode;

std::filesystem::path current_executable() {
  std::uint32_t size = 0;
  (void)::_NSGetExecutablePath(nullptr, &size);
  std::vector<char> buffer(size + 1, '\0');
  if (::_NSGetExecutablePath(buffer.data(), &size) != 0) {
    throw WorkerError(WorkerErrorCode::io, "cannot locate the running svp-builder");
  }
  std::error_code error;
  const std::filesystem::path path = std::filesystem::canonical(buffer.data(), error);
  if (error) {
    throw WorkerError(WorkerErrorCode::io, "cannot resolve the running svp-builder");
  }
  return path;
}

svp::exec::worker::CoordinatorHello CoordinatorContext::hello() const {
  return svp::exec::worker::make_coordinator_hello(runtime, thread_plan, model_set);
}

CoordinatorContext load_coordinator_context() {
  CoordinatorContext context;
  context.runtime = svp::exec::worker::locate_coordinator_runtime(current_executable());
  // Until a build hands its own plan to the session (later task types), the
  // handshake carries the plan a local build on this Mac would resolve with
  // one OCR recognition worker; HELLO only checks that it is host-independent.
  context.thread_plan = svp::models::resolve_local_thread_plan(
      svp::models::detect_host_cpu_topology(), /*ocr_recognition_workers=*/1);
  context.model_cache = svp::models::model_cache_root();
  try {
    context.model_set = svp::exec::worker::model_set_summary(context.model_cache);
  } catch (const std::exception&) {
    context.model_set.reset();
  }
  return context;
}

}  // namespace svp::builder::workers
