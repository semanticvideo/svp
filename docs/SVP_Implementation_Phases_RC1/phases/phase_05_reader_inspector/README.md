# Phase 05 - Inspector

## Phase purpose

Build the package inspection surface that can read SVP packages without validating everything. This tool becomes the human and agent debugging layer for fixtures and the builder.

The separate `svp-reader` executable has been retired as a phase output. Reader responsibilities now live in library APIs (`packages/svp-package` and `packages/svp-query`) and are exposed through the single `svp-inspector` CLI. This keeps package debugging under one command instead of splitting dump and inspect behavior across two binaries.

## Prerequisites

- Phase 02 complete.
- Preferably Phase 04 has at least one valid fixture.

## Primary outputs

```text
tools/svp-inspector
packages/svp-package reader APIs
packages/svp-query package query APIs
human-readable package summaries
```

## Work items

1. Implement package open/read APIs in `packages/svp-package`.
2. Add manifest reading.
3. Add registry of package sections.
4. Add transcript record counting.
5. Add shot/scene count reading.
6. Add binary block metadata listing.
7. Add SQLite table summary.
8. Build `svp-inspector inspect package.svp` with concise output.
9. Build `svp-inspector dump package.svp --section manifest` and `--section index_manifest`.

## Required commands

```bash
cmake --build build
./build/tools/svp-inspector/svp-inspector inspect fixtures/phase-04-ocr-color/valid/visible_ui_text.svp
./build/tools/svp-inspector/svp-inspector dump fixtures/phase-04-ocr-color/valid/visible_ui_text.svp --section manifest
./build/tools/svp-inspector/svp-inspector dump fixtures/phase-04-ocr-color/valid/visible_ui_text.svp --section index_manifest
```

## Definition of done

- Inspector can summarize at least one fixture.
- Inspector can dump manifest and index manifest.
- The tool does not crash on invalid packages. It should report partial-read status.

## Handoff report format

At the end of this phase, report:

```text
Phase: Phase 05 - Inspector
Status: complete / blocked / partial
Files created:
Files modified:
Commands run:
Tests passed:
Tests failed:
Known issues:
Next recommended phase:
```
