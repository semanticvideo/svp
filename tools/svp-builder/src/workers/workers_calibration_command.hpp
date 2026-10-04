#pragma once

// The capacity measurements `workers pair`, `workers sync`, and `workers
// fleet pair` take for one paired worker (default_build_calibration.hpp):
// every task type a default --distributed build dispatches, on this Mac and
// on the worker, so the builds that follow measure nothing.

#include "svp/exec/worker/hello_messages.hpp"
#include "svp/exec/worker/pairing_store.hpp"
#include "svp/exec/worker/runtime_source.hpp"

#include <filesystem>

namespace svp::builder::workers {

// Measures (or confirms) every type and prints one line for each Mac and
// type. Returns false, after saying why, when OCR could not be measured on
// either Mac; any other type that cannot be measured is reported, and a
// --distributed build measures it.
bool calibrate_for_workers_command(const svp::exec::worker::CoordinatorPairingRecord& record,
                                   const svp::exec::worker::CoordinatorHello& hello,
                                   const svp::exec::worker::CoordinatorRuntime& runtime,
                                   const std::filesystem::path& model_cache);

}  // namespace svp::builder::workers
