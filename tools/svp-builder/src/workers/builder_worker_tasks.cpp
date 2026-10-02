#include "builder_worker_tasks.hpp"

#include "worker_model_view.hpp"

#include "../batch/video_build_task.hpp"

#include "svp/audio/tasks/audio_task_environment.hpp"
#include "svp/vision/tasks/dispatched_vision_tasks.hpp"
#include "svp/vision/tasks/ocr_frame_batch_task.hpp"
#include "svp/vision/tasks/track_window_task.hpp"

#include <memory>
#include <span>
#include <string>

namespace svp::builder::workers {

void register_builder_worker_task_types(svp::exec::TaskTypeRegistry& registry,
                                        svp::exec::CasTaskArtifactAccess& artifacts,
                                        const WorkerTaskEnvironment& environment) {
  auto models = std::make_shared<WorkerModelView>(environment.model_store,
                                                  environment.session_dir);
  // OCR frame batches and evidence-crop batches load the same PP-OCR
  // sessions; one pool serves both.
  auto pp_ocr_sessions = std::make_shared<svp::vision::tasks::PpOcrSessionPool>();
  const auto write_output = [&artifacts](std::span<const std::byte> bytes,
                                         std::string media_type, std::string role) {
    return artifacts.put(bytes, std::move(media_type), std::move(role));
  };
  const auto model_cache_for = [models](const svp::exec::TaskSpec& spec) {
    return models->cache_for(spec);
  };
  svp::vision::tasks::register_ocr_frame_batch_task(
      registry,
      svp::vision::tasks::OcrFrameBatchWorkerEnvironment{.model_cache_root = {},
                                                         .ffmpeg_path = environment.ffmpeg_path,
                                                         .write_output = write_output,
                                                         .model_cache_for = model_cache_for},
      pp_ocr_sessions);
  // The dispatched vision stage work (M4). A task that cannot start here
  // fails retryably, so another Mac takes it (record_start_failures false).
  svp::vision::tasks::register_dispatched_vision_tasks(
      registry,
      svp::vision::tasks::DispatchedTaskEnvironment{.model_cache_root = {},
                                                    .model_cache_for = model_cache_for,
                                                    .ffmpeg_path = environment.ffmpeg_path,
                                                    .scratch_dir = environment.session_dir,
                                                    .write_output = write_output,
                                                    .record_start_failures = false},
      pp_ocr_sessions);
  // ASR chunks and diarization windows (M5). An item that cannot run here,
  // or goes wrong here, fails retryably so another Mac takes it.
  svp::audio::tasks::register_audio_tasks(
      registry, svp::audio::tasks::AudioTaskEnvironment{.model_cache_root = {},
                                                        .model_cache_for = model_cache_for,
                                                        .scratch_dir = environment.session_dir,
                                                        .write_output = write_output,
                                                        .record_start_failures = false});
  // Tracking windows (M4). A window that cannot start here, or that goes
  // wrong here, fails retryably so another Mac takes it.
  svp::vision::tasks::register_track_window_task(
      registry, svp::vision::tasks::TrackWindowWorkerEnvironment{
                    .model_cache_root = {},
                    .ffmpeg_path = environment.ffmpeg_path,
                    .write_output = write_output,
                    .model_cache_for = model_cache_for});
  // Whole-video jobs of another Mac's batch (M6).
  svp::builder::batch::register_video_build_task(
      registry, artifacts,
      svp::builder::batch::VideoBuildWorkerEnvironment{
          .session_dir = environment.session_dir,
          .cas_root = environment.cas_root,
          .worker_session_id = environment.worker_session_id,
          .models = models,
          .tools = environment.tools});
}

}  // namespace svp::builder::workers
