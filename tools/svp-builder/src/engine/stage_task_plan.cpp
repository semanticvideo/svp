#include "engine/stage_task_plan.hpp"

#include "svp/exec/cache_key.hpp"
#include "svp/exec/parameters_digest.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>

namespace svp::builder::engine {
namespace {

// The reducer lane all whole-stage tasks share; ordinals are graph positions.
constexpr std::string_view kStageOrderLane = "pipeline";

constexpr std::string_view kProcessorsJsonl = "provenance/processors.jsonl";

StagingScope scope_for(StageTaskKind kind, bool microphone_stream_mode) {
  switch (kind) {
    case StageTaskKind::inventory:
    case StageTaskKind::vision_plan:
    case StageTaskKind::canonical_frames:
    case StageTaskKind::package_write:
    case StageTaskKind::media_binding:
      return {};
    case StageTaskKind::color:
      return {{"colors/", "timeline/", std::string(kProcessorsJsonl)}};
    case StageTaskKind::foundation_ocr:
      return {{"text/", std::string(kProcessorsJsonl)}};
    case StageTaskKind::audio_extract:
      return {{"media/audio/", std::string(kProcessorsJsonl)}};
    case StageTaskKind::audio_transcribe:
      if (microphone_stream_mode) {
        return {{"transcript/", std::string(kProcessorsJsonl)}};
      }
      return {{"transcript/"}};
    case StageTaskKind::depth:
      return {{"spatial/depth."}};
    case StageTaskKind::ocr:
      return {{"text/"}};
    case StageTaskKind::text_embeddings:
      return {{"embeddings/"}};
    case StageTaskKind::tracking:
      return {{"spatial/masks.", "spatial/regions.", "entities/",
               std::string(kProcessorsJsonl)}};
    case StageTaskKind::entities:
      return {{std::string(kProcessorsJsonl), "timeline/frames.jsonl", "entities/"}};
    case StageTaskKind::relationships:
      return {{"relationships/", "provenance/"}};
    case StageTaskKind::index:
      return {{"index/"}};
    case StageTaskKind::validation:
      return {{"provenance/validation.json"}};
    case StageTaskKind::svpi_write:
      return {{"provenance/", "index/"}};
  }
  throw std::logic_error("unknown stage task kind");
}

using DependencyMap = std::map<StageTaskKind, std::vector<StageTaskKind>>;

void plan_package_lanes(const StageTaskPlanInputs& inputs,
                        std::vector<StageTaskKind>& order, DependencyMap& deps) {
  using K = StageTaskKind;
  const std::vector<K> audio_lane{K::audio_extract, K::audio_transcribe};
  const std::vector<K> vision_lane{K::canonical_frames, K::depth, K::ocr,
                                   K::text_embeddings, K::tracking};
  deps[K::audio_transcribe] = {K::audio_extract};
  deps[K::depth] = {K::canonical_frames};
  deps[K::ocr] = {K::canonical_frames};
  deps[K::text_embeddings] = {K::ocr};
  deps[K::tracking] = {K::depth, K::text_embeddings};

  if (inputs.serial_pipeline) {
    // Today's serial order: the vision lane (depth before OCR), then audio.
    order.insert(order.end(), vision_lane.begin(), vision_lane.end());
    order.insert(order.end(), audio_lane.begin(), audio_lane.end());
    deps[K::canonical_frames] = {K::color};
    deps[K::ocr].push_back(K::depth);
    deps[K::audio_extract] = {K::color, K::tracking};
  } else if (inputs.single_video_heavy_lanes > 1) {
    order.insert(order.end(), audio_lane.begin(), audio_lane.end());
    order.insert(order.end(), vision_lane.begin(), vision_lane.end());
    deps[K::audio_extract] = {K::color};
    deps[K::canonical_frames] = {K::color};
    // provenance/processors.jsonl writers: extraction rewrites the file, the
    // microphone path appends to it after ASR, tracking merges into it.
    deps[K::tracking].push_back(K::audio_extract);
    if (inputs.microphone_stream_mode) {
      deps[K::tracking].push_back(K::audio_transcribe);
    }
  } else {
    // One heavy lane: the audio lane, then the vision lane.
    order.insert(order.end(), audio_lane.begin(), audio_lane.end());
    order.insert(order.end(), vision_lane.begin(), vision_lane.end());
    deps[K::audio_extract] = {K::color};
    deps[K::canonical_frames] = {K::color, K::audio_transcribe};
  }

  const std::vector<K> final_stages{K::entities, K::relationships, K::index,
                                    K::validation, K::package_write};
  order.insert(order.end(), final_stages.begin(), final_stages.end());
  // The join reads every lane's results (states and staging files).
  deps[K::entities] = {K::color, K::audio_transcribe, K::canonical_frames, K::depth,
                       K::ocr, K::text_embeddings, K::tracking};
  deps[K::relationships] = {K::entities};
  deps[K::index] = {K::entities, K::relationships};
  deps[K::validation] = {K::entities, K::index};
  deps[K::package_write] = {K::entities, K::validation};

  if (inputs.svpi_publication) {
    order.push_back(K::media_binding);
    order.push_back(K::svpi_write);
    deps[K::media_binding] = {K::package_write};
    deps[K::svpi_write] = {K::package_write, K::media_binding};
  }
}

// reachable[a][b]: a path of dependencies leads from b to a (b runs first).
std::vector<std::vector<bool>> transitive_closure(
    const std::vector<PlannedStageTask>& tasks) {
  const std::size_t n = tasks.size();
  std::map<StageTaskKind, std::size_t> index;
  for (std::size_t i = 0; i < n; ++i) {
    index[tasks[i].kind] = i;
  }
  std::vector<std::vector<bool>> reachable(n, std::vector<bool>(n, false));
  // Tasks are in topological order, so dependencies are already closed.
  for (std::size_t i = 0; i < n; ++i) {
    for (const StageTaskKind dependency : tasks[i].depends_on) {
      const std::size_t d = index.at(dependency);
      if (d >= i) {
        throw std::logic_error("stage task plan is not in topological order");
      }
      reachable[i][d] = true;
      for (std::size_t k = 0; k < n; ++k) {
        if (reachable[d][k]) {
          reachable[i][k] = true;
        }
      }
    }
  }
  return reachable;
}

void check_scope_ordering(const std::vector<PlannedStageTask>& tasks) {
  const auto reachable = transitive_closure(tasks);
  for (std::size_t i = 0; i < tasks.size(); ++i) {
    for (std::size_t j = 0; j < i; ++j) {
      if (!reachable[i][j] && scopes_overlap(tasks[i].scope, tasks[j].scope)) {
        throw std::logic_error(std::string("stage tasks `") +
                               std::string(stage_task_id(tasks[i].kind)) + "` and `" +
                               std::string(stage_task_id(tasks[j].kind)) +
                               "` may run concurrently but share staging paths");
      }
    }
  }
}

}  // namespace

std::string_view stage_task_id(StageTaskKind kind) noexcept {
  switch (kind) {
    case StageTaskKind::inventory: return "task.inventory.source_000";
    case StageTaskKind::color: return "task.color.vstream_000";
    case StageTaskKind::vision_plan: return "task.vision_plan.vstream_000";
    case StageTaskKind::foundation_ocr: return "task.ocr.vstream_000.foundation";
    case StageTaskKind::audio_extract: return "task.audio.extract.source_000";
    case StageTaskKind::audio_transcribe: return "task.transcript.source_000";
    case StageTaskKind::canonical_frames: return "task.frames.vstream_000.canonical";
    case StageTaskKind::depth: return "task.depth.vstream_000.canonical";
    case StageTaskKind::ocr: return "task.ocr.vstream_000";
    case StageTaskKind::text_embeddings: return "task.embedding.text.vstream_000";
    case StageTaskKind::tracking: return "task.tracking.vstream_000";
    case StageTaskKind::entities: return "task.entities.package";
    case StageTaskKind::relationships: return "task.relationships.package";
    case StageTaskKind::index: return "task.index.package";
    case StageTaskKind::validation: return "task.validation.package";
    case StageTaskKind::package_write: return "task.package.write";
    case StageTaskKind::media_binding: return "task.binding.source_000";
    case StageTaskKind::svpi_write: return "task.svpi.write";
  }
  return "task.unknown";
}

std::string_view stage_task_type(StageTaskKind kind) noexcept {
  switch (kind) {
    case StageTaskKind::inventory: return "inventory.media_ingest";
    case StageTaskKind::color: return "color.timeline";
    case StageTaskKind::vision_plan: return "vision.plan";
    case StageTaskKind::foundation_ocr: return "ocr.foundation";
    case StageTaskKind::audio_extract: return "audio.extract";
    case StageTaskKind::audio_transcribe: return "audio.transcribe";
    case StageTaskKind::canonical_frames: return "frames.canonical";
    case StageTaskKind::depth: return "depth.canonical_frames";
    case StageTaskKind::ocr: return "ocr.observations";
    case StageTaskKind::text_embeddings: return "embedding.text";
    case StageTaskKind::tracking: return "tracking.visual_entities";
    case StageTaskKind::entities: return "package.entities";
    case StageTaskKind::relationships: return "package.relationships";
    case StageTaskKind::index: return "package.index";
    case StageTaskKind::validation: return "package.validation";
    case StageTaskKind::package_write: return "package.write";
    case StageTaskKind::media_binding: return "svpi.media_binding";
    case StageTaskKind::svpi_write: return "svpi.write";
  }
  return "unknown";
}

std::vector<PlannedStageTask> plan_stage_tasks(const StageTaskPlanInputs& inputs) {
  using K = StageTaskKind;
  std::vector<K> order{K::inventory};
  DependencyMap deps;
  const BuildStageExecutionPlan& stage_plan = inputs.stage_plan;

  if (stage_plan.run_package_skeleton) {
    order.push_back(K::color);
    deps[K::color] = {K::inventory};
    plan_package_lanes(inputs, order, deps);
  } else {
    // A --stop-after build runs one foundation stage, in today's order.
    K previous = K::inventory;
    const auto chain = [&](K kind) {
      order.push_back(kind);
      deps[kind] = {previous};
      previous = kind;
    };
    if (stage_plan.run_audio) {
      chain(K::audio_extract);
      chain(K::audio_transcribe);
    }
    if (stage_plan.run_vision_plan) chain(K::vision_plan);
    if (stage_plan.run_foundation_color) chain(K::color);
    if (stage_plan.run_foundation_ocr) chain(K::foundation_ocr);
  }

  std::vector<PlannedStageTask> tasks;
  tasks.reserve(order.size());
  for (const K kind : order) {
    tasks.push_back(PlannedStageTask{
        .kind = kind,
        .depends_on = deps[kind],
        .scope = scope_for(kind, inputs.microphone_stream_mode)});
  }
  check_scope_ordering(tasks);
  return tasks;
}

svp::exec::TaskGraph make_stage_task_graph(const std::vector<PlannedStageTask>& tasks,
                                           const std::string& build_session_id,
                                           const std::string& build_inputs_blake3) {
  std::vector<svp::exec::TaskNode> nodes;
  nodes.reserve(tasks.size());
  for (std::size_t ordinal = 0; ordinal < tasks.size(); ++ordinal) {
    const PlannedStageTask& task = tasks[ordinal];
    svp::exec::TaskSpec spec;
    spec.build_session_id = build_session_id;
    spec.task_id = std::string(stage_task_id(task.kind));
    spec.task_type = std::string(stage_task_type(task.kind));
    spec.task_type_version = kStageTaskTypeVersion;
    for (const StageTaskKind dependency : task.depends_on) {
      spec.depends_on.emplace_back(stage_task_id(dependency));
    }
    std::sort(spec.depends_on.begin(), spec.depends_on.end());
    spec.depends_on.erase(std::unique(spec.depends_on.begin(), spec.depends_on.end()),
                          spec.depends_on.end());
    spec.parameters = {{"build_inputs_blake3", build_inputs_blake3},
                       {"staging_scope", task.scope.prefixes}};
    spec.parameters_blake3 = svp::exec::compute_parameters_blake3(spec.parameters);
    // RC2 §20.3 key over what fixes a whole-stage task's output: its type and
    // version and its parameters (which carry the build inputs digest).
    spec.cache_key = svp::exec::compute_cache_key(nlohmann::json::array(
        {std::string(svp::exec::kCacheKeyDomain), spec.task_type,
         spec.task_type_version, svp::exec::blake3_hex(spec.parameters_blake3)}));
    // Whole-stage tasks are not placed by estimates: the executor has a slot
    // for every task that can run at once. est_cpu_threads is the minimum a
    // valid spec carries.
    spec.resources = svp::exec::TaskResources{
        .est_peak_rss_mb = 0, .est_cpu_threads = 1, .est_seconds = 0};
    nodes.push_back(svp::exec::TaskNode{
        .spec = std::move(spec),
        .order_key = svp::exec::TaskOrderKey{.lane = std::string(kStageOrderLane),
                                             .ordinals = {ordinal}}});
  }
  return svp::exec::TaskGraph(std::move(nodes));
}

std::size_t stage_task_graph_width(const std::vector<PlannedStageTask>& tasks) {
  // Dilworth: width = tasks - maximum matching in the comparability graph
  // (u -> v when u must run before v).
  const auto reachable = transitive_closure(tasks);
  const std::size_t n = tasks.size();
  std::vector<std::size_t> match_of_later(n, n);
  const std::function<bool(std::size_t, std::vector<bool>&)> augment =
      [&](std::size_t earlier, std::vector<bool>& seen) {
        for (std::size_t later = 0; later < n; ++later) {
          if (!reachable[later][earlier] || seen[later]) {
            continue;
          }
          seen[later] = true;
          if (match_of_later[later] == n || augment(match_of_later[later], seen)) {
            match_of_later[later] = earlier;
            return true;
          }
        }
        return false;
      };
  std::size_t matching = 0;
  for (std::size_t earlier = 0; earlier < n; ++earlier) {
    std::vector<bool> seen(n, false);
    if (augment(earlier, seen)) {
      ++matching;
    }
  }
  return n - matching;
}

}  // namespace svp::builder::engine
