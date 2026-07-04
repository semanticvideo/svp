# SVP Implementation Roadmap

The implementation should proceed in this order.

## Phase 1: Repository Setup

- Commit SVP v1.0 RC1 spec.
- Commit schemas.
- Commit registries.
- Commit companion docs.
- Preserve original release package under `releases/`.

## Phase 2: Validator

Build `svp-validator` first.

The validator should support:

```bash
svp validate path/to/package.svp
svp validate --json path/to/package.svp
svp validate --equivalent package-a.svp package-b.svp
```

Initial validator scope:

1. Open `.svp` as ZIP64.
2. Validate required package layout.
3. Validate required JSON files against schemas.
4. Validate registry files.
5. Parse SVPB binary block headers.
6. Verify BLAKE3 hashes.
7. Validate `index/index_manifest.json`.
8. Open `index/index.sqlite`.
9. Generate canonical SQLite logical row stream.
10. Compare `logical_rows_blake3`.
11. Emit machine-readable validation output.

## Phase 3: Fixtures

Create tiny fixture packages by hand before building the full builder.

Required fixtures:

- static-card
- vertical-video
- square-video
- silent-video
- single-speaker
- overlapping-speech
- screen-recording
- moving-object
- degenerate-depth
- cpu-vs-gpu-equivalence fixtures

The purpose of fixtures is to make validator behavior testable.

## Phase 4: Package reader APIs

Build package reader APIs after the validator.

Reader responsibilities:

- open package
- read manifest
- enumerate required sections
- read transcript records
- read shot/scene records
- read binary block metadata
- read index manifest
- expose package summary

## Phase 5: Inspector

Build `svp-inspector` on top of the reader APIs. Do not add a separate
`svp-reader` executable unless a future machine-facing use case needs a distinct
binary; manifest and index-manifest dump behavior belongs in `svp-inspector`.

Inspector responsibilities:

- print human-readable package summary
- dump manifest and index manifest
- list tracks
- list speech regions
- list blocks
- list validation state
- inspect index metadata

## Phase 6: Builder

Build `svp-builder` last.

The builder is the hardest piece. It should not begin until the validator and basic fixtures exist.

Builder responsibilities:

- ingest source video
- extract original media/audio
- create transcript and word timestamps
- detect speakers
- detect shots/scenes
- create visual entity tracks/spatial regions from motion and depth evidence
- create depth/mask/embedding blocks
- preserve entity identity through deterministic tracker fixtures
- create SQLite index
- create index manifest
- write package
- validate output
