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

This repository currently contains:

- SVP v1.0 Release Candidate 1 specification
- Machine-readable registries
- JSON schemas
- Companion specs for index manifests, model bundles, and detached signature sidecars
- Review notes and diff from Draft 0.6 to RC1

The next implementation milestone is `svp-validator`, not the full package builder.

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

## Implementation Order

1. Set up the spec repo.
2. Build `svp-validator`.
3. Create minimal fixture packages.
4. Build `svp-reader`.
5. Build `svp-inspector`.
6. Build `svp-builder`.

Do not start with the full video-to-SVP builder. The validator must exist first so packages can be tested.
