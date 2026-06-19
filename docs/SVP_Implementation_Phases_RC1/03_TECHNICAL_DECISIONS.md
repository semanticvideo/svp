# Technical Decisions for RC1 Implementation

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
packages/svp-builder-core
```

The repo already has the first six folders. Add the last four when builder work begins.

## First implementation priority

The first real implementation priority is the validator.

The builder must not be the first serious code because a builder without a validator produces unverifiable artifacts.

## File format rule

The spec is the source of truth. Do not change RC1 semantics while implementing unless a true spec bug is discovered. Spec bugs should be filed as issues or notes for RC2, not silently patched in code.

## No Python runtime rule

The reference builder must not require Python for normal operation.

It may use Python in development scripts, fixture generation helpers, or one-off repo tooling, but not as a runtime dependency for `svp build`.
