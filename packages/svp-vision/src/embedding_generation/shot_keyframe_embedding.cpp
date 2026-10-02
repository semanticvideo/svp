#include "shot_keyframe_embedding.hpp"

#include "svp/models/manifest.hpp"
#include "svp/models/runtime.hpp"
#include "svp/models/verification.hpp"
#include "svp/vision/canonical_frame_input.hpp"
#include "svp/vision/dispatched_work.hpp"
#include "svp/vision/vision_embedding_input.hpp"

#include <opencv2/imgproc.hpp>

#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <thread>
#include <utility>

namespace svp::vision::detail {
namespace {

std::vector<nlohmann::json> read_jsonl(const std::filesystem::path& path) {
  std::vector<nlohmann::json> records;
  std::ifstream input(path);
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) continue;
    try {
      records.push_back(nlohmann::json::parse(line));
    } catch (...) {
    }
  }
  return records;
}

// Decoded keyframes waiting for inference. Bounds memory to a fixed number of
// 224x224 inputs however many shots the video has; two lets the next decode
// finish while one keyframe is in inference.
constexpr std::size_t kKeyframeDecodeLookahead = 2;

struct KeyframeInput {
  std::size_t keyframe_index = 0;
  std::vector<float> input;  // CHW model input; empty when decoding missed
};

class KeyframeInputQueue {
 public:
  // Blocks while the queue is full; drops the item after cancel().
  void push(KeyframeInput item) {
    std::unique_lock lock(mutex_);
    space_.wait(lock, [&] {
      return cancelled_ || items_.size() < kKeyframeDecodeLookahead;
    });
    if (cancelled_) return;
    items_.push_back(std::move(item));
    ready_.notify_one();
  }

  // Blocks until an item arrives; nullopt once closed and drained.
  std::optional<KeyframeInput> pop() {
    std::unique_lock lock(mutex_);
    ready_.wait(lock, [&] { return closed_ || !items_.empty(); });
    if (items_.empty()) return std::nullopt;
    KeyframeInput item = std::move(items_.front());
    items_.pop_front();
    space_.notify_one();
    return item;
  }

  void close() {
    std::lock_guard lock(mutex_);
    closed_ = true;
    ready_.notify_all();
  }

  void cancel() {
    std::lock_guard lock(mutex_);
    cancelled_ = true;
    space_.notify_all();
  }

  bool cancelled() {
    std::lock_guard lock(mutex_);
    return cancelled_;
  }

 private:
  std::mutex mutex_;
  std::condition_variable ready_;
  std::condition_variable space_;
  std::deque<KeyframeInput> items_;
  bool closed_ = false;
  bool cancelled_ = false;
};

KeyframeEmbeddingItem embedding_item(const ShotKeyframe& keyframe) {
  return KeyframeEmbeddingItem{.shot_id = keyframe.shot_id,
                               .pts_us = keyframe.pts_us,
                               .width = keyframe.analysis_width,
                               .height = keyframe.analysis_height};
}

// Loads and verifies the vision model into `session`, or sets
// result.blocker; on success fills the result's model identity.
void load_vision_model(const ShotKeyframeEmbeddingRequest& request,
                       ShotKeyframeEmbeddings& result,
                       svp::models::OnnxSession& session) {
  std::optional<svp::models::ModelBundleManifest> manifest_opt;
  try {
    manifest_opt = svp::models::load_model_bundle_manifest(
        request.model_bundle_dir / "model.svpmodel.json");
    if (!svp::models::verify_manifest_files(*manifest_opt,
                                            request.model_bundle_dir)
             .ok()) {
      result.blocker = "vision model bundle failed BLAKE3 verification";
    } else {
      svp::models::OnnxSessionOptions session_options;
      session_options.execution_provider = request.execution_provider;
      session_options.threads = request.threads;
      session = svp::models::OnnxSession::load(
          *manifest_opt, request.model_bundle_dir, session_options);
    }
  } catch (const std::exception& e) {
    result.blocker = std::string("vision model could not be loaded: ") + e.what();
  } catch (...) {
    result.blocker = "vision model could not be loaded";
  }
  if (!result.blocker.empty()) {
    return;
  }
  result.model_id = manifest_opt->model_id;
  result.model_bundle_id = manifest_opt->model_bundle_id;
  result.model_blake3 = manifest_opt->bundle_blake3.hex_value();
}

// The keyframes embedded by request.dispatcher (dispatched_work.hpp): the
// model is loaded and verified here first, so its blockers are this Mac's;
// a keyframe the dispatcher could not embed is embedded again here. nullopt
// when the dispatcher hands the keyframes back to the stage.
std::optional<ShotKeyframeEmbeddings> embed_dispatched_keyframes(
    const std::vector<ShotKeyframe>& keyframes,
    const ShotKeyframeEmbeddingRequest& request) {
  ShotKeyframeEmbeddings result;
  result.keyframes_requested = keyframes.size();
  svp::models::OnnxSession session;
  load_vision_model(request, result, session);
  if (!result.blocker.empty()) {
    return result;
  }
  std::vector<KeyframeEmbeddingItem> items;
  items.reserve(keyframes.size());
  for (const ShotKeyframe& keyframe : keyframes) {
    items.push_back(embedding_item(keyframe));
  }
  std::optional<std::vector<KeyframeEmbeddingOutcome>> outcomes = request.dispatcher(
      items,
      DispatchedModel{.model_id = result.model_id,
                      .execution_provider = request.execution_provider,
                      .threads = request.threads},
      request.embedding_dim,
      [&request](std::size_t done, std::size_t total) {
        if (request.on_keyframe) request.on_keyframe(done, total);
      });
  if (!outcomes) {
    return std::nullopt;
  }
  if (outcomes->size() != items.size()) {
    throw DispatchedWorkError("keyframe embeddings: " + std::to_string(outcomes->size()) +
                              " outcomes for " + std::to_string(items.size()) +
                              " keyframes");
  }
  for (std::size_t index = 0; index < items.size(); ++index) {
    KeyframeEmbeddingOutcome& outcome = (*outcomes)[index];
    if (outcome.embedded && outcome.vector.size() != request.embedding_dim) {
      throw DispatchedWorkError("keyframe embeddings: vector for " + items[index].shot_id +
                                " has " + std::to_string(outcome.vector.size()) +
                                " values, not " + std::to_string(request.embedding_dim));
    }
    if (!outcome.embedded) {
      outcome = embed_keyframe(session, request.ffmpeg_path, request.media_plan->source_path,
                               items[index], request.embedding_dim);
    }
    if (outcome.embedded) {
      result.vectors.push_back({items[index].shot_id, std::move(outcome.vector)});
    }
  }
  if (result.vectors.empty()) {
    result.blocker = "no shot keyframe could be decoded and embedded";
  }
  return result;
}

}  // namespace

std::vector<ShotKeyframe> load_shot_keyframes(
    const std::filesystem::path& staging_dir) {
  struct FrameFacts {
    std::int64_t pts_us = 0;
    int width = 0;
    int height = 0;
  };
  std::map<std::string, FrameFacts> frames;
  for (const auto& frame :
       read_jsonl(staging_dir / "timeline" / "frames.jsonl")) {
    const std::string id = frame.value("id", "");
    if (id.empty() || !frame.contains("pts_us")) continue;
    frames[id] = {frame["pts_us"].get<std::int64_t>(),
                  frame.value("analysis_width", 0),
                  frame.value("analysis_height", 0)};
  }

  std::vector<ShotKeyframe> keyframes;
  for (const auto& shot :
       read_jsonl(staging_dir / "timeline" / "shots.jsonl")) {
    const std::string shot_id = shot.value("id", "");
    const std::string frame_id = shot.value("start_frame_id", "");
    const auto it = frames.find(frame_id);
    if (shot_id.empty() || it == frames.end() || it->second.width <= 0 ||
        it->second.height <= 0) {
      continue;
    }
    keyframes.push_back({shot_id, frame_id, it->second.pts_us,
                         it->second.width, it->second.height});
  }
  return keyframes;
}

ShotKeyframeEmbeddings embed_shot_keyframes(
    const std::vector<ShotKeyframe>& keyframes,
    const ShotKeyframeEmbeddingRequest& request) {
  ShotKeyframeEmbeddings result;
  result.keyframes_requested = keyframes.size();
  if (keyframes.empty()) {
    result.blocker = "no shot keyframes in timeline/shots.jsonl";
    return result;
  }
  if (request.media_plan == nullptr || request.ffmpeg_path.empty()) {
    result.blocker = "no media plan or ffmpeg available to decode keyframes";
    return result;
  }
  if (request.dispatcher) {
    if (std::optional<ShotKeyframeEmbeddings> dispatched =
            embed_dispatched_keyframes(keyframes, request)) {
      return std::move(*dispatched);
    }
  }

  // Decoding (one ffmpeg seek per keyframe) runs on a producer thread so it
  // overlaps model loading and inference; vectors still come out in keyframe
  // order because the single consumer pops them in that order.
  KeyframeInputQueue queue;
  std::thread producer([&] {
    for (std::size_t i = 0; i < keyframes.size() && !queue.cancelled(); ++i) {
      const auto& keyframe = keyframes[i];
      KeyframeInput item{i, {}};
      try {
        item.input = keyframe_model_input(request.ffmpeg_path,
                                          request.media_plan->source_path,
                                          embedding_item(keyframe));
      } catch (...) {
        item.input.clear();
      }
      queue.push(std::move(item));  // an empty input marks a decode miss
    }
    queue.close();
  });

  svp::models::OnnxSession session;
  load_vision_model(request, result, session);
  if (!result.blocker.empty()) {
    queue.cancel();
    producer.join();
    return result;
  }

  // The producer must be joined on every exit, including a throwing
  // progress callback, or ~thread terminates the process.
  try {
    std::size_t completed = 0;
    while (auto item = queue.pop()) {
      if (std::optional<std::vector<float>> vector =
              embed_keyframe_input(session, item->input, request.embedding_dim)) {
        result.vectors.push_back(
            {keyframes[item->keyframe_index].shot_id, std::move(*vector)});
      }
      ++completed;
      if (request.on_keyframe) request.on_keyframe(completed, keyframes.size());
    }
  } catch (...) {
    queue.cancel();
    producer.join();
    throw;
  }
  producer.join();
  if (result.vectors.empty()) {
    result.blocker = "no shot keyframe could be decoded and embedded";
  }
  return result;
}

}  // namespace svp::vision::detail
