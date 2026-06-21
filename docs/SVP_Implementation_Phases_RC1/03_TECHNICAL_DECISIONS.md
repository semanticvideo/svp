# Technical Decisions for RC2 Implementation

These are the implementation choices for the first serious implementation pass.

## Language and build system

Use:

```text
C++20
CMake
vcpkg manifest mode
```

Reason: this matches the native reference-builder direction and avoids Python runtime dependency for end users.

## Top-level dependency set

Use these dependencies first:

```text
CLI11              command-line parsing
nlohmann-json      JSON parsing and report writing
SQLite3            index.sqlite access
libzip             .svp ZIP64 package reading and writing
BLAKE3             hashing
zstd               SVPB payload decompression/compression
FFmpeg             media probing, extraction, frame/audio decode
OpenCV             classical CV, optical flow, tracking, masks, shot metrics
ONNX Runtime       local model execution
```

Add `whisper.cpp` as a vendored third-party dependency or submodule for transcription if the team chooses not to shell out to a bundled executable. Normal operation must not require Python.

RC2 adds first-class OCR / visible-text observations and structured color observations. OCR may use model-backed detection and recognition through SVP Model Bundles. Structured color should start as deterministic classical processing: color-space normalization, registry-backed bucket quantization, and numeric coverage summaries for scenes, shots, frames/keyframes, regions/entities, and text regions.

## Executable naming

Early phase executables can be separate:

```text
svp-validator
svp-reader
svp-inspector
svp-builder
```

Later, add a unified CLI wrapper:

```bash
svp validate ...
svp inspect ...
svp build ...
```

## Shared packages

Use these internal packages:

```text
packages/svp-core
packages/svp-package
packages/svp-schema
packages/svp-blocks
packages/svp-index
packages/svp-validation
packages/svp-media
packages/svp-audio
packages/svp-vision
packages/svp-models
packages/svp-text
packages/svp-color
packages/svp-builder-core
```

The repo already has the foundation folders. Add later packages only when their owning phase begins and the RC2 OCR/color unblock checklist is satisfied.

## First implementation priority

The first real implementation priority is the validator.

The builder must not be the first serious code because a builder without a validator produces unverifiable artifacts.

## File format rule

The spec is the source of truth. Do not change RC2 semantics while implementing unless a true spec bug is discovered. Spec bugs should be filed as issues or notes for a later clarification, not silently patched in code.

Visible text, numeric values extracted from visible text, and measured color distributions are Core observations in RC2. Do not implement them as labels.

## Coverage and constants rule

Correctness behavior must not rely on unexplained magic numbers or sample-specific patches. Any constant that affects validity, observation coverage, sampling cadence, thresholds, caps, confidence, timing, dimensions, or model identity must be named, owned, documented, and tested as a policy.

Temporal and spatial observation layers need explicit coverage contracts. OCR frame sampling, ASR chunking, VAD segmentation, scene/shot segmentation, color sampling, relationship overlap checks, and model/runtime limits must record the strategy and limitations in provenance when the strategy can miss events. A fixed sample count is not a coverage contract.

Agents must not hard-code source filenames, expected text, specific timestamps, frame indexes, hand-measured crop boxes, or one-off thresholds to make a trial video pass. Ground truth may be used for scoring and reports, but production behavior must be general.

## No Python runtime rule

The reference builder must not require Python for normal operation.

It may use Python in development scripts, fixture generation helpers, or one-off repo tooling, but not as a runtime dependency for `svp build`.
