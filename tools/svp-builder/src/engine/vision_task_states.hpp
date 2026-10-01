#pragma once

// State names of the vision lane tasks, shared by the vision tasks that
// produce them and the package entities task that joins them.

namespace svp::builder::engine::vision_state {

inline constexpr const char* kCanonicalFramesIndex = "canonical_frames_index";
inline constexpr const char* kCanonicalFramePixels = "canonical_frame_pixels";
inline constexpr const char* kDepth = "vision_depth";
inline constexpr const char* kOcr = "vision_ocr";
inline constexpr const char* kTextEmbeddings = "vision_text_embeddings";
inline constexpr const char* kTracking = "vision_tracking";

}  // namespace svp::builder::engine::vision_state
