#include "onnx_model_pool.hpp"

#include "svp/models/verification.hpp"

namespace svp::vision::tasks::detail {
namespace {

// The bundle manifest's file name in a model cache (the stages' lookup).
constexpr const char* kManifestFilename = "model.svpmodel.json";
// The manifest file role of the text model's WordPiece vocabulary.
constexpr const char* kTokenizerVocabRole = "tokenizer_vocab";

std::string request_key(const OnnxModelRequest& request) {
  return request.model_cache_root.string() + "\n" + request.model_id + "\n" +
         request.execution_provider + "\n" + std::to_string(request.threads.intra_op) + "/" +
         std::to_string(request.threads.inter_op) + "\n" +
         (request.with_tokenizer ? "tokenizer" : "");
}

std::unique_ptr<LoadedOnnxModel> load(const OnnxModelRequest& request) {
  auto loaded = std::make_unique<LoadedOnnxModel>();
  if (!svp::models::OnnxSession::is_available()) {
    loaded->problem = "ONNX Runtime is not available in this build";
    return loaded;
  }
  loaded->bundle_dir = request.model_cache_root / request.model_id;
  try {
    loaded->manifest =
        svp::models::load_model_bundle_manifest(loaded->bundle_dir / kManifestFilename);
    if (loaded->manifest->model_id != request.model_id) {
      loaded->problem = "bundle at " + loaded->bundle_dir.string() + " is " +
                        loaded->manifest->model_id + ", not " + request.model_id;
      return loaded;
    }
    if (!svp::models::verify_manifest_files(*loaded->manifest, loaded->bundle_dir).ok()) {
      loaded->problem = "model bundle " + request.model_id + " failed BLAKE3 verification";
      return loaded;
    }
    if (request.with_tokenizer) {
      std::filesystem::path vocab;
      for (const auto& file : loaded->manifest->files) {
        if (file.role == kTokenizerVocabRole) {
          vocab = loaded->bundle_dir / file.path;
          break;
        }
      }
      if (vocab.empty() || !loaded->tokenizer.load(vocab)) {
        loaded->problem = "tokenizer vocabulary of " + request.model_id + " could not be loaded";
        return loaded;
      }
    }
    svp::models::OnnxSessionOptions options;
    options.execution_provider = request.execution_provider;
    options.threads = request.threads;
    loaded->session =
        svp::models::OnnxSession::load(*loaded->manifest, loaded->bundle_dir, options);
  } catch (const std::exception& error) {
    loaded->problem = "model " + request.model_id + " could not be loaded: " + error.what();
  }
  return loaded;
}

}  // namespace

OnnxModelPool::Lease::Lease(OnnxModelPool& pool, std::string key,
                            std::unique_ptr<LoadedOnnxModel> model)
    : pool_(pool), key_(std::move(key)), model_(std::move(model)) {}

OnnxModelPool::Lease::~Lease() {
  if (model_->problem.empty()) {
    pool_.release(key_, std::move(model_));
  }
}

std::unique_ptr<OnnxModelPool::Lease> OnnxModelPool::acquire(const OnnxModelRequest& request) {
  std::string key = request_key(request);
  {
    const std::lock_guard lock(mutex_);
    auto found = idle_.find(key);
    if (found != idle_.end() && !found->second.empty()) {
      std::unique_ptr<LoadedOnnxModel> model = std::move(found->second.back());
      found->second.pop_back();
      return std::make_unique<Lease>(*this, std::move(key), std::move(model));
    }
  }
  // Loading takes seconds; it never holds the pool's lock.
  return std::make_unique<Lease>(*this, key, load(request));
}

void OnnxModelPool::clear_idle() {
  std::map<std::string, std::vector<std::unique_ptr<LoadedOnnxModel>>> idle;
  {
    const std::lock_guard lock(mutex_);
    idle.swap(idle_);
  }
  // Sessions are destroyed outside the lock.
}

void OnnxModelPool::release(const std::string& key, std::unique_ptr<LoadedOnnxModel> model) {
  const std::lock_guard lock(mutex_);
  idle_[key].push_back(std::move(model));
}

}  // namespace svp::vision::tasks::detail
