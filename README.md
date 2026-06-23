# SVP: Semantic Video Package

SVP is an open standard for strict, machine-readable semantic packaging of time-based media.

A `.svp` package is not an AI plugin format and not merely an MP4 wrapper. It is a fully baked media observation package containing required media, audio, transcript, word timing, speaker timing, shot and scene boundaries, visual tracks, spatial regions, relationships, depth, masks, embeddings, search index, provenance, validation metadata, and manifest data.

The philosophy is simple:

- SVP Core stores observations, not interpretations.
- Labels are non-core.
- AI may consume SVP, but AI does not define SVP.
- A conforming `.svp` package must be strict, complete, inspectable, and validator-testable.
- The reference builder is machinery. The package format is the standard.

## Current Status

SVP v1.0 RC2 is the active implementation target. The repository now contains
the spec, validator, package writer, query/inspection tools, and a real builder
path that can produce validator-clean `.svp` packages from local sample media
when the required native tools and model cache are available.

The current implementation includes:

- Machine-readable registries and JSON schemas for RC2 package sections.
- `svp-validator`, `svp-inspector`, and `svp-builder` CLIs.
- Media probing, audio extraction, ONNX Whisper ASR, word timings, speaker
  records, diarization, and transcript provenance.
- Native PP-OCR ONNX visible-text and numeric-value observations.
- Structured color observations, sampled timeline frames/shots/scenes, depth
  blocks, text embeddings, SQLite index output, relationship records, and
  stored validation reports.
- Hardened model/runtime behavior for ONNX Runtime, PP-OCR model identity,
  sherpa-onnx library discovery, ASR confidence provenance, and diarization
  fallback validation.

Recent real-video proof paths include `test-30.mp4`, `SVP-TEST.MOV`,
`CODE.MOV`, `intro.mp4`, and `speakers.MOV`. The known current gap is no longer
basic package validity; it is completing the remaining semantic layers and
hardening quality, query traversal, entities/tracks, masks/regions, and
validator coverage.

## Repository Layout

```text
docs/                 Human-readable project docs
spec/                 SVP v1.0 RC1 spec, schemas, registries, companion docs
fixtures/             Future test fixtures for validators/readers/builders
tools/                CLI tools: validator, reader, inspector, builder
packages/             Shared libraries/modules
releases/             Archived release packages
scripts/              Utility scripts
```

## Active Implementation Focus

The original validator-first milestone has been met. Current work should remain
narrow, reviewable, and validator-backed. The next important lanes are:

1. Entity and entity-track artifacts.
2. Relationship traversal queries over package graph output.
3. Mask and spatial-region foundations.
4. Timeline/scene quality hardening beyond sampled-frame interval evidence.
5. Transcript, vision, and embedding quality improvements.
6. Strict validator coverage for speaker, diarization, relationship, entity,
   model-bundle, and package-hygiene rules.

Do not replace validator-backed package work with hand-assembled output. A
builder claim is complete only when the generated package is inspectable and
the validator passes.
