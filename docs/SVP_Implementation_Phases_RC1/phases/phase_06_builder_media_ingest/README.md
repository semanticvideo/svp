# Phase 06 - Builder Media Ingest and Canonical Timing

## Phase purpose

Start the actual builder by implementing deterministic media probing, source copying, audio extraction, canonical analysis raster computation, and rational timestamp conversion.

## Prerequisites

- Phase 01 complete.
- Phase 02 validator core complete.

## Primary outputs

```text
packages/svp-media
tools/svp-builder build command
FFmpeg probe integration
source media copy
lossless audio extraction
canonical raster computation
canonical timestamp utilities
partial package skeleton writer
```

## Work items

1. Add `packages/svp-media`.
2. Integrate FFmpeg or ffprobe for stream probing.
3. Read duration, frame rate, timebase, resolution, rotation, pixel aspect ratio, display aspect ratio, audio streams.
4. Implement exact rational timestamp conversion to integer microseconds with round-half-to-even.
5. Compute canonical analysis raster with longest displayed dimension 640 and shorter dimension rounded to nearest even.
6. Extract audio to required WAV format for downstream audio pipeline.
7. Copy or store original media according to RC2 package layout.
8. Write a partial package skeleton with manifest and media files.
9. Mark incomplete packages clearly during build. Do not claim full core validity yet.

## Required commands

```bash
cmake --build build
./build/tools/svp-builder/svp-builder build samples/dom-30s.mov --out out/dom-30s.partial.svp --stop-after media-ingest
./build/tools/svp-inspector/svp-inspector inspect out/dom-30s.partial.svp
```

## Definition of done

- Builder can probe a real video.
- Builder extracts audio.
- Builder computes canonical raster correctly for landscape, vertical, and square samples.
- Builder writes partial package skeleton.
- Timestamp conversion has unit tests for 24000/1001, 30000/1001, 25/1, 30/1, and 60/1.

## Handoff report format

At the end of this phase, report:

```text
Phase: Phase 06 - Builder Media Ingest and Canonical Timing
Status: complete / blocked / partial
Files created:
Files modified:
Commands run:
Tests passed:
Tests failed:
Known issues:
Next recommended phase:
```
