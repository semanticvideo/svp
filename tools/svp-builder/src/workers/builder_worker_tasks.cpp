#include "builder_worker_tasks.hpp"

#include "worker_model_view.hpp"

#include "svp/vision/tasks/ocr_frame_batch_task.hpp"

#include <memory>
#include <span>
#include <string>

namespace svp::builder::workers {

void register_builder_worker_task_types(svp::exec::TaskTypeRegistry& registry,
                                        svp::exec::CasTaskArtifactAccess& artifacts,
                                        const WorkerTaskEnvironment& environment) {
  auto models = std::make_shared<WorkerModelView>(environment.model_store,
                                                  environment.session_dir);
  svp::vision::tasks::register_ocr_frame_batch_task(
      registry, svp::vision::tasks::OcrFrameBatchWorkerEnvironment{
                    .model_cache_root = {},
                    .ffmpeg_path = environment.ffmpeg_path,
                    .write_output =
                        [&artifacts](std::span<const std::byte> bytes, std::string media_type,
                                     std::string role) {
                          return artifacts.put(bytes, std::move(media_type), std::move(role));
                        },
                    .model_cache_for =
                        [models](const svp::exec::TaskSpec& spec) {
                          return models->cache_for(spec);
                        }});
}

}  // namespace svp::builder::workers
