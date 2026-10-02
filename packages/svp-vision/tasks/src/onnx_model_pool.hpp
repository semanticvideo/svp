#pragma once

// ONNX model sessions kept alive across tasks of one runtime, for the
// dispatched vision task types that load one model each (embed.text_batch:
// nomic-embed-text; embed.keyframe_batch: nomic-embed-vision;
// depth.frame_batch: Depth Anything V2). Like PpOcrSessionPool, a session is
// reused only by a later task with the same session settings (model cache
// root, model id, execution provider, thread counts), so reuse cannot change
// output; each running task holds its own session, so the pool grows to the
// number of tasks this runtime runs at once. A model that fails to load is
// never kept, so a later task retries the load. Thread-safe.
//
// A model is loaded exactly as its stage loads it: the bundle at
// <cache>/<model_id>, its manifest parsed and every file verified against
// its BLAKE3 before ONNX Runtime opens it, and for the text model the
// tokenizer vocabulary the manifest names.

#include "svp/models/manifest.hpp"
#include "svp/models/runtime.hpp"
#include "svp/vision/text_tokenizer.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace svp::vision::tasks::detail {

struct OnnxModelRequest {
  std::filesystem::path model_cache_root;
  std::string model_id;
  std::string execution_provider;
  svp::models::OrtThreadCounts threads;
  bool with_tokenizer = false;
};

struct LoadedOnnxModel {
  // Empty when the model could not be loaded here.
  std::string problem;
  // Set once the manifest parsed.
  std::optional<svp::models::ModelBundleManifest> manifest;
  std::filesystem::path bundle_dir;
  svp::models::OnnxSession session;
  WordPieceTokenizer tokenizer;
};

class OnnxModelPool {
 public:
  class Lease {
   public:
    Lease(OnnxModelPool& pool, std::string key, std::unique_ptr<LoadedOnnxModel> model);
    ~Lease();
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;

    [[nodiscard]] const LoadedOnnxModel& model() const { return *model_; }

   private:
    OnnxModelPool& pool_;
    std::string key_;
    std::unique_ptr<LoadedOnnxModel> model_;
  };

  // An idle model loaded with the same request, or a newly loaded one (whose
  // `problem` says why it could not be loaded).
  [[nodiscard]] std::unique_ptr<Lease> acquire(const OnnxModelRequest& request);

  // Destroys every idle model (their memory is returned); leased ones are
  // unaffected.
  void clear_idle();

 private:
  void release(const std::string& key, std::unique_ptr<LoadedOnnxModel> model);

  std::mutex mutex_;
  std::map<std::string, std::vector<std::unique_ptr<LoadedOnnxModel>>> idle_;
};

}  // namespace svp::vision::tasks::detail
