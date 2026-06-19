# Phase 04 - Fixture Lab: Minimal Valid and Invalid SVP Packages

## Phase purpose

Create controlled `.svp` fixtures that make validator behavior testable. These are not full real-world packages yet. They are small, intentional conformance fixtures.

## Prerequisites

- Phase 02 complete for report shape.
- Phase 03 in progress or complete for deep validation.

## Primary outputs

```text
fixtures/static-card/example.svp
fixtures/silent-video/example.svp
fixtures/vertical-video/example.svp
fixtures/invalid/missing-depth.svp
fixture build scripts
fixture README docs
```

## Work items

1. Create a fixture generation helper under `fixtures/tools` or `scripts`.
2. Generate tiny ZIP64-compatible `.svp` packages with minimal JSON/JSONL content.
3. Include valid fixture for static card with empty tracks and near-uniform depth/masks.
4. Include silent-video fixture with VAD regions absent or no words, depending on spec behavior.
5. Include vertical-video fixture to validate canonical raster dimensions.
6. Include deliberate invalid fixtures: missing required file, bad block hash, invalid index manifest, wrong canonical raster, unsupported label schema warning.
7. Add fixture README explaining the purpose of every fixture.
8. Make validator tests consume these fixtures.

## Required commands

```bash
python3 fixtures/tools/make_minimal_fixtures.py
./build/tools/svp-validator/svp-validator validate fixtures/static-card/example.svp --json
./build/tools/svp-validator/svp-validator validate fixtures/invalid/missing-depth.svp --json
```

## Definition of done

- At least three valid fixtures exist.
- At least three invalid fixtures exist.
- Validator produces expected statuses for each.
- Fixture generation is reproducible.
- Fixture files are small enough to commit.

## Handoff report format

At the end of this phase, report:

```text
Phase: Phase 04 - Fixture Lab: Minimal Valid and Invalid SVP Packages
Status: complete / blocked / partial
Files created:
Files modified:
Commands run:
Tests passed:
Tests failed:
Known issues:
Next recommended phase:
```
