#include "svp/models/runtime.hpp"

#include "svp/models/error.hpp"

namespace svp::models {

OnnxSession OnnxSession::load(const ModelBundleManifest&,
                              const std::filesystem::path&,
                              const OnnxSessionOptions&) {
  throw ModelError(ModelErrorCode::runtime_unavailable,
                   "ONNX Runtime support is not configured in this build; model "
                   "bundle parsing and BLAKE3 verification are available");
}

}  // namespace svp::models
