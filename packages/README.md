# SVP Packages / Libraries

Shared implementation modules should live here.

Current modules:

## svp-core

Core data types, IDs, timestamps, hash utilities, and common enums.

## svp-media

Media probing, canonical timing, rational timebase handling, and ingest plans.

## svp-audio

Audio extraction planning/execution, waveform envelopes, VAD/ASR execution
boundaries, Whisper transcript records, speaker records, diarization
boundaries, and diarization fallback reporting.

The full `.svp` build path may stage extracted audio streams and analysis WAV
files because SVP is a self-contained package. SVPI sidecars must not package
replayable source-derived audio; that filtering happens in `svp-package`.

## svp-vision

OCR/text observations, numeric extraction, evidence crops, color observations,
frame sampling, depth generation, embeddings, masks, and visual entity support.

## svp-models

Model cache, manifest, hash, verification, lock-file, and ONNX Runtime session
handling.

## svp-package

ZIP64 package reading/writing and required layout handling for both `.svp` and
`.svpi`.

Important responsibilities include:

- Full SVP package writing with embedded `media/original/`.
- SVPI package writing without `media/original/`.
- SVPI media policy for forbidden replayable derivatives.
- Media binding creation/parsing/verification.
- Timeline, entity, relationship, provenance, validation-report, and index
  foundation writers.

SVPI media policy lives in:

```text
packages/svp-package/include/svp/package/svpi_media_policy.hpp
packages/svp-package/src/svpi_media_policy.cpp
```

The policy intentionally allows non-replayable observations such as
`media/audio/waveform.jsonl`, `media/audio/audio_absence.json`, transcript
records, OCR evidence crops, block streams, SQLite indexes, and JSON/JSONL
records while rejecting replayable audio/video/muxed media by path and
case-insensitive media extension.

## svp-blocks

SVPB binary block header parsing, validation, and hash verification.

## svp-query

SQLite-backed query reading, package layer listing, transcript/OCR/color query
helpers, relationship listing, graph traversal, node summaries, and graph
health diagnostics.

## svp-validation

Validation report model, validation code registry, status computation, SVP
package validation, SVPI structure validation, index checks, text/color checks,
diarization checks, block-stream checks, and equivalence reporting.

SVPI validation currently enforces:

- required `mimetype`, `manifest.json`, `media_binding.json`, provenance, and
  index spine;
- `manifest.json` with `format: "svpi"`;
- `svpi.media_identity.v0.1` binding contract;
- no `media/original/`;
- no replayable source-derived audio/video/muxed media
  derivatives.
