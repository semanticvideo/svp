#pragma once

// --coordinators for every command that builds a batch of videos
// (build-batch, interlace create-batch; M6): the other Macs, among this
// Mac's paired workers, that may each coordinate one video of the batch at a
// time, and what they build with (this Mac's thread plan, ffmpeg build,
// runtime, and model bundles), so a video's package does not depend on which
// Mac built it.

#include "svp/builder/remote_video_builder.hpp"
#include "svp/vision/inference_performance.hpp"

#include <CLI/CLI.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

void add_coordinators_option(CLI::App& command, std::vector<std::string>& coordinators);

struct CliBatchCoordinators {
  std::vector<std::shared_ptr<svp::builder::batch::RemoteVideoBuilder>> macs;
  // thread_plan and ffmpeg_build filled; the command fills the rest.
  svp::builder::batch::VideoBuildParameters parameters;
};

// Empty when no --coordinators were given. nullopt (after printing why) when
// they cannot be used: an entry matches no pairing, this Mac cannot assemble
// what they need, or this platform has no paired workers; the command then
// exits 2.
[[nodiscard]] std::optional<CliBatchCoordinators> make_cli_batch_coordinators(
    const std::vector<std::string>& coordinators,
    const svp::vision::InferencePerformanceOptions& performance,
    const std::filesystem::path& model_cache_dir, const std::string& ffmpeg_path, bool quiet,
    std::string_view command_name);
