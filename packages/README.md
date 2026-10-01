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

## svp-exec

Execution contracts for distributed builds: `ArtifactRef`, `TaskSpec`, and
`TaskResult` records with their canonical JSON codecs, `parameters_blake3`,
`cache_key` (RC2 §20.3), and `output_digest` constructions, the
transport-independent frame codec, and the task type registry. It also holds
the build storage layer: the global content-addressed cache (`CasStore`, RC2
§20.3 roots, `blobs/b3/<2 hex>/<62 hex>`, verified writes, LRU eviction with
pins, RC2 §20.5.2 non-fatal errors) and the RC2 §20.4 recovery journal
(`RecoveryJournal`: `<output>-journal/` with `build.sqlite` in WAL mode,
verified artifact commits, resume, and §20.5.1 cleanup).

On Apple platforms `svp-exec/remote` adds the TLS-PSK transport with Bonjour
discovery and the `RemoteExecutor`, and `svp-exec/worker` adds worker pairing
and the worker agent: the HELLO / runtime / blob protocol, memory admission,
the pairing store, launchd job and install-script generation, the runtime and
model-bundle stores on a worker, and the agent session that runs a verified
runtime's session process (`svp-builder workers ...`, `svp-builder worker
serve`).

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
