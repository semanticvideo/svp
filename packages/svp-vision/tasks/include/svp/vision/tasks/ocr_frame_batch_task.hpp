#pragma once

#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/task_registry.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>
#include <string>

namespace svp::vision::tasks {

// What a runtime provides to ocr.frame_batch beyond the TaskSpec: its own
// model cache and ffmpeg (never sent by the coordinator, plan §4.3: no
// message carries a path), and the store its outputs go to.
struct OcrFrameBatchWorkerEnvironment {
  std::filesystem::path model_cache_root;
  std::filesystem::path ffmpeg_path;
  // Stores output bytes in the runtime's TaskArtifactAccess store (for
  // example CasTaskArtifactAccess::put) and returns their ref.
  std::function<svp::exec::ArtifactRef(std::span<const std::byte> bytes,
                                       std::string media_type, std::string role)>
      write_output;
};

// Registers ocr.frame_batch version 1 with its strict parameter validator.
//
// The task decodes the spec's samples from its `source` input and runs PP-OCR
// on each, returning one OcrSampleDetections JSONL output
// (kOcrFrameDetectionsRole), one record per sample in parameter order. Before
// running it requires the spec's model_refs to name exactly the parameters'
// detector and recognizer, and the worker's loaded bundles to match those
// refs' bundle_blake3. PP-OCR sessions are pooled for the registry's lifetime
// (see PpOcrSessionPool). Cancellation is checked between frames.
//
// Failures that another worker may not share are retryable failed results:
// ocr_unavailable (PP-OCR could not load here), model_mismatch (this worker's
// bundles differ from the refs), decode_unavailable (no usable ffmpeg here).
// A spec whose model_refs disagree with its parameters is invalid_model_refs,
// permanent. Per-frame decode misses and PP-OCR errors are data in the
// payload, as they are for a local build.
void register_ocr_frame_batch_task(svp::exec::TaskTypeRegistry& registry,
                                   OcrFrameBatchWorkerEnvironment environment);

}  // namespace svp::vision::tasks
