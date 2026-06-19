# Phase 03 - Deep Validator: Schemas, SVPB Blocks, Hashes, and SQLite Logical Integrity

## Phase purpose

Move the validator from basic layout checks to real package conformance checks. This phase makes the validator useful for actual `.svp` packages and fixture development.

## Prerequisites

- Phase 02 complete.
- Basic validator can open ZIP packages and emit reports.

## Primary outputs

```text
JSON schema validation
SVPB binary block parser
BLAKE3 hash verification
index/index_manifest.json validation
SQLite logical row stream generator
logical_rows_blake3 comparison
```

## Work items

1. Add JSON schema validation for the schemas currently in `spec/schemas`.
2. Add `packages/svp-blocks` for SVPB header parsing.
3. Parse SVPB block streams for depth, masks, embeddings.
4. Verify magic, version, header size, block type, compression enum, dtype, dimensions, frame range, and payload sizes.
5. Compute BLAKE3 for payloads and compare exact-governed fields.
6. Add `packages/svp-index`.
7. Open `index/index.sqlite` read-only.
8. Implement canonical SQLite logical row stream per RC1.
9. Compute `logical_rows_blake3` and compare against `index/index_manifest.json`.
10. Respect RC1 equivalence rules for tolerance-governed derived columns. Do not compare hashes with tolerance. Defer hash mismatches to source-layer equivalence where required.
11. Validate signature sidecar only if explicitly provided, and keep authenticity findings separate from core status.

## Required commands

```bash
cmake --build build
./build/tools/svp-validator/svp-validator validate fixtures/static-card/example.svp --json
./build/tools/svp-validator/svp-validator validate fixtures/silent-video/example.svp --json
```

## Definition of done

- Validator can parse SVPB headers.
- Validator can open SQLite index.
- Validator can compute logical row hash.
- Validator reports specific registered errors for missing/corrupt depth, masks, embeddings, index manifest, and SQLite mismatches.
- Authenticity findings do not affect core status by default.

## Handoff report format

At the end of this phase, report:

```text
Phase: Phase 03 - Deep Validator: Schemas, SVPB Blocks, Hashes, and SQLite Logical Integrity
Status: complete / blocked / partial
Files created:
Files modified:
Commands run:
Tests passed:
Tests failed:
Known issues:
Next recommended phase:
```
