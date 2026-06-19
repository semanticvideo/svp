# Phase 05 - Reader and Inspector

## Phase purpose

Build tools that can read and inspect SVP packages without validating everything. These tools become the human debugging layer for fixtures and the builder.

## Prerequisites

- Phase 02 complete.
- Preferably Phase 04 has at least one valid fixture.

## Primary outputs

```text
tools/svp-reader
tools/svp-inspector
packages/svp-package reader APIs
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
9. Build `svp-reader dump package.svp --section manifest` or equivalent.

## Required commands

```bash
cmake --build build
./build/tools/svp-inspector/svp-inspector inspect fixtures/static-card/example.svp
./build/tools/svp-reader/svp-reader dump fixtures/static-card/example.svp --section manifest
```

## Definition of done

- Inspector can summarize at least one fixture.
- Reader can dump manifest and index manifest.
- Tools do not crash on invalid packages. They should report partial-read status.

## Handoff report format

At the end of this phase, report:

```text
Phase: Phase 05 - Reader and Inspector
Status: complete / blocked / partial
Files created:
Files modified:
Commands run:
Tests passed:
Tests failed:
Known issues:
Next recommended phase:
```
