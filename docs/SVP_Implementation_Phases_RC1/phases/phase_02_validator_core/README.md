# Phase 02 - Validator Core and Machine-Readable Reports

## Phase purpose

Implement the first serious validator path: open inputs, load registries, emit structured reports, validate basic package layout, and map findings to registered validation codes.

## Prerequisites

- Phase 01 complete.
- `svp-validator` binary builds.

## Primary outputs

```text
packages/svp-validation report model
packages/svp-package package probe
validator JSON output
basic layout validation
registry loading
```

## Work items

1. Load `spec/registries/validation-codes.json` at runtime.
2. Add validation report model matching RC1 structure: `status`, `core_status`, `authenticity_status`, `errors`, `warnings`, `infos`, `authenticity`.
3. Implement exit code rules: 0 valid/valid_with_warnings, 1 invalid core, 2 unreadable/runtime error.
4. Implement file existence and `.svp` extension checks.
5. Implement ZIP opening with libzip.
6. Check required top-level package entries from RC1.
7. Emit JSON with `--json`.
8. Emit concise human-readable output without `--json`.
9. Add tests or smoke commands for missing file, wrong extension, non-ZIP `.svp`, and empty ZIP.

## Required commands

```bash
cmake --build build
./build/tools/svp-validator/svp-validator validate missing.svp --json
./build/tools/svp-validator/svp-validator validate README.md --json
./build/tools/svp-validator/svp-validator validate path/to/empty.svp --json
```

## Definition of done

- Missing file produces structured report and exit code 2.
- Wrong extension produces structured report.
- Non-ZIP `.svp` produces structured report.
- Registry loads successfully.
- Findings use registered codes or documented `X_VALIDATOR_` temporary codes.
- Validator does not crash on bad inputs.

## Handoff report format

At the end of this phase, report:

```text
Phase: Phase 02 - Validator Core and Machine-Readable Reports
Status: complete / blocked / partial
Files created:
Files modified:
Commands run:
Tests passed:
Tests failed:
Known issues:
Next recommended phase:
```
