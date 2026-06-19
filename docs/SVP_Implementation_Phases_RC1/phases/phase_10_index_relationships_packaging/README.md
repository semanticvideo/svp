# Phase 10 - Relationships, Embeddings, SQLite Index, Index Manifest, and Final Package Writer

## Phase purpose

Finish the core package assembly path. This phase turns media/audio/vision observations into a complete package with relationships, embeddings, search index, index manifest, provenance, and validation output.

## Prerequisites

- Phase 07 complete.
- Phase 09 complete.
- Phase 03 validator can validate blocks and SQLite logical integrity.
- Phase 08 model runtime can generate embeddings.

## Primary outputs

```text
relationships/relationships.jsonl
embeddings/embeddings.blocks.svpez
index/index.sqlite
index/index_manifest.json
provenance/validation.json
atomic package writer
builder finalization flow
```

## Work items

1. Add `packages/svp-builder-core` if not already present.
2. Generate time-based relationships: word in speaker segment, shot contains words, scene contains shots, track visible during shot, speaker visible while speaking if track association exists.
3. Generate geometry relationships: region overlap, track enters frame, track exits frame, track near camera if depth supports it.
4. Generate text embeddings for transcript chunks.
5. Generate visual embeddings for frame/shot chunks if reference model is available.
6. Write embedding SVPB stream.
7. Build SQLite index with required tables.
8. Generate canonical SQLite logical row stream.
9. Write `index/index_manifest.json` with `logical_rows_blake3`.
10. Write provenance records.
11. Run validator at end of build and write `provenance/validation.json`.
12. Ensure package finalization is atomic.

## Required commands

```bash
cmake --build build
./build/tools/svp-builder/svp-builder build samples/dom-30s.mov --out out/dom-30s.svp
./build/tools/svp-validator/svp-validator validate out/dom-30s.svp --json
./build/tools/svp-inspector/svp-inspector inspect out/dom-30s.svp
```

## Definition of done

- Builder produces a complete `.svp` file.
- Validator can open it and emit a full report.
- Index manifest exists and logical hash is computed.
- Embedding blocks exist and parse.
- Relationships exist and are inspectable.
- Build does not require Python runtime.

## RC2 OCR and color delta

Phase 10 must include SQLite and query paths for:

- text regions
- recognized raw text
- normalized text
- numeric values
- text region targets and OCR target references
- color observations
- scene color bucket percentages
- shot color bucket percentages
- frame/keyframe color bucket percentages
- region/entity color bucket percentages
- text-region color bucket percentages
- text-region foreground/background color

Update `index_manifest.logical_rows_blake3` and the canonical logical row stream procedure to include the new OCR/color tables.

Queries like "find all orange scenes" must be supported by indexed scene or shot color bucket percentages.

Visible text, normalized text, numeric values, and measured color coverage are Core observation index inputs. Do not index them only as labels, captions, embeddings, or display-only inspector strings.

## Handoff report format

At the end of this phase, report:

```text
Phase: Phase 10 - Relationships, Embeddings, SQLite Index, Index Manifest, and Final Package Writer
Status: complete / blocked / partial
Files created:
Files modified:
Commands run:
Tests passed:
Tests failed:
Known issues:
Next recommended phase:
```
