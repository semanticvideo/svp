## Worktree Rules

- Use a worktree only when the repo owner or orchestrator assigns one.
- Keep generated build folders, caches, and local outputs inside the assigned checkout/worktree or ignored paths.
- Do not modify another worktree to fix your own build.
- Report the checkout or worktree path in the handoff.

## Shared Contract Zones

Coordinate before changing these files or folders:

- `CMakeLists.txt`
- `vcpkg.json`
- `packages/svp-core`
- `packages/svp-validation`
- `spec/registries/validation-codes.json`

These files define shared behavior. If your assigned task needs them, say so in the handoff and keep changes narrow.

## File Design

- Avoid god files.
- Prefer one-job files with clear ownership.
- Split code by responsibility before files become difficult to review.
- Do not hide unrelated helpers, policies, formatters, or shared logic inside a convenient large file.
- Keep public interfaces small and boring.

## No Magic-Number Fixes

- Do not fix a real bug by hard-coding a number, timestamp, sample count, frame index, filename, text string, model threshold, path, or dimension that only solves the observed sample.
- Any constant that affects correctness, coverage, validity, sampling, timing, thresholds, limits, confidence, hashes, package size, or runtime behavior must have a named owner and a documented reason.
- Prefer explicit policy fields, options, registries, manifests, or shared constants over literals buried in function calls.
- When behavior depends on temporal or spatial coverage, define the coverage contract in code and provenance. For example, frame sampling must explain maximum sample gap, caps, skipped regions, and what events can be missed.
- Tests must prove the general policy, not the one video, timestamp, or phrase that exposed the bug.
- Reviewers must treat unexplained literals in correctness paths as findings, especially in OCR, ASR, diarization, scene segmentation, color sampling, relationship validation, model verification, and package validation.

## Testing Rules

Run tests or verification commands when code, build files, schemas, fixtures, scripts, or behavior changed.

Do not run tests just to perform ceremony. For example, a reviewer does not need to rerun a passing build unless they changed code, suspect the previous test result is invalid, or need evidence for a finding.

When tests are run, report:

- The exact command.
- Whether it passed or failed.
- Any important output or failure reason.

If code changed and tests could not be run, explain why.

## SVP Project Priorities

- SVP v1.0 RC2 is the active implementation target.
- The validator path is the spine of the project.
- The full builder comes after validator and fixtures are useful.
- The spec is the source of truth.
- Do not silently change RC2 semantics. Document true spec issues for later review.
- Normal `svp build` operation must not require Python.
- Do not continue Phase 03+, fixture lab, media ingest, vision pipeline, relationships/index packaging, or first-video-trial work until OCR and structured color observations are represented in the validator, fixtures, builder roadmap, index/query model, and first-trial acceptance criteria.
- Visible text, numeric values extracted from visible text, and measured color coverage are Core observations in RC2. Do not treat them as labels.
