# Phase 09 - Vision Observations: Shots, Tracks, Regions, Depth, and Masks

## Phase purpose

Implement the visual observation path. This is one of the core SVP value engines: canonical frames, shot boundaries, tracks, regions, depth blocks, and mask blocks.

## Prerequisites

- Phase 06 complete.
- Phase 08 complete enough to run Depth Anything V2 Small or test ONNX models.
- Phase 03 validator can parse SVPB blocks.

## Primary outputs

```text
packages/svp-vision
shot boundary detection
scene boundary records
entity tracks
spatial regions
depth.blocks.svpdz
masks.blocks.svpmz
vision provenance
```

## Work items

1. Add `packages/svp-vision`.
2. Decode frames at deterministic cadence from source video.
3. Generate canonical analysis raster frames.
4. Implement shot boundary detection using deterministic frame-difference metrics first, then improve.
5. Emit scene records. For first pass, scenes can be derived from shot groups.
6. Implement optical flow and motion metrics with OpenCV.
7. Implement track candidates using contours, foreground regions, optical-flow clusters, and Kalman smoothing.
8. Emit entity tracks without labels.
9. Emit spatial regions attached to tracks and frames.
10. Generate depth blocks using Depth Anything V2 Small through model runtime.
11. Generate masks from tracked regions. Use conservative masks rather than hallucinated segmentation.
12. Write SVPB block streams with BLAKE3 hashes.
13. Validate generated blocks with `svp-validator`.

## Required commands

```bash
cmake --build build
./build/tools/svp-builder/svp-builder build samples/dom-30s.mov --out out/dom-30s.vision.svp --stop-after vision
./build/tools/svp-validator/svp-validator validate out/dom-30s.vision.svp --json
```

## Definition of done

- Shot records exist.
- Scene records exist.
- Depth block stream exists and parses.
- Mask block stream exists and parses.
- Entity track and region files exist.
- Degenerate/static video produces valid empty tracks and near-uniform depth/mask behavior where appropriate.

## RC2 OCR and color delta

Phase 09 now includes independent OCR and structured color builder stages:

```text
ocr_text_detection
ocr_text_recognition
ocr_layout_classification
ocr_numeric_extraction
ocr_text_region_color_sampling
color_space_normalization
color_bucket_quantization
shot_color_summary
scene_color_summary
frame_color_summary
region_color_summary
entity_color_summary
text_region_color_summary
text_region_foreground_background_color
```

Color processing should be deterministic classical processing first. It should produce numeric bucket coverage for scenes, shots, frames or keyframes, regions/entities, and text regions where applicable.

OCR may be model-backed through SVP Model Bundles. Do not hardcode OCR into unrelated vision stages; keep text detection, recognition, layout classification, and numeric extraction separately owned.

Phase 09 output must feed RC2 Core `/text/` and `/colors/` records: detected text regions, recognized visible text, normalized text, numeric values, honest text absence, registry-backed color observations, scene/shot/frame/region/entity/text-region color bucket percentages, text-region foreground/background colors, and honest color absence when coverage cannot be measured.

## Handoff report format

At the end of this phase, report:

```text
Phase: Phase 09 - Vision Observations: Shots, Tracks, Regions, Depth, and Masks
Status: complete / blocked / partial
Files created:
Files modified:
Commands run:
Tests passed:
Tests failed:
Known issues:
Next recommended phase:
```
