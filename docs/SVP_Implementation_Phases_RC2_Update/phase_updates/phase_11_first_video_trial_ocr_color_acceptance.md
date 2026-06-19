# Phase 11 Delta: First 30-Second Video Trial Acceptance

The first real video trial must produce a Core-valid package that includes `/text/` and `/colors/`.

Acceptance checks:

- `svp validate` passes.
- The package contains the RC2 `/text/` files required by the spec.
- The package contains the RC2 `/colors/` files required by the spec.
- `svp inspect` reports text region counts, text observation counts, color observation counts, and scene/shot color summary counts.
- Trial evidence includes text/count proof from `/text/`.
- Trial evidence includes color/count proof from `/colors/`.
- At least one scene or shot color summary is queryable.
- Scene/shot color percentages support proof queries like "find all orange scenes."
- If the test video contains visible text, it appears in `text_observations` and the index.
- If visible text contains a number, a numeric value record is produced when safe.
- If no visible text appears, `text_absence.json` honestly records that absence.
- If color coverage cannot be measured, `color_absence.json` honestly records that absence.
