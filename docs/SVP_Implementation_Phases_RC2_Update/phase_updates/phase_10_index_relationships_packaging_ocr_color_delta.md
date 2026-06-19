# Phase 10 Delta: Text and Color Index Integration

Add SQLite tables and query paths for text regions, visible recognized text, normalized text, numeric values, text region targets, color observations, color bucket percentages, and text-region foreground/background color lookups.

Color query planning must support scene, shot, frame/keyframe, region/entity, and text-region bucket percentages so queries like "find all orange scenes" are answered from structured color observations, not labels or embeddings alone.

Update the canonical logical row stream and `index_manifest.logical_rows_blake3` procedure to include the new OCR/color tables. Any package writer change that adds, removes, reorders, or normalizes OCR/color rows must update the logical hash inputs in the same scope.
