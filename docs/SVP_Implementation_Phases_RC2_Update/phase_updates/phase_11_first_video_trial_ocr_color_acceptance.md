# Phase 11 Delta: First 30-Second Video Trial Acceptance

The first real video trial must produce a Core-valid package that includes `/text/` and `/colors/`.

Acceptance checks:

- `svp validate` passes.
- `svp inspect` reports text region counts and color observation counts.
- At least one scene/shot color summary is queryable.
- If the test video contains visible text, it appears in `text_observations` and the index.
- If visible text contains a number, a numeric value record is produced when safe.
