# SVP Agent Operating Contract

This repository is coordinated through a Codex orchestrator thread. Builder and reviewer agents must follow this file before doing any work.

## Orchestration Loop

- The current orchestrator thread owns sequencing, assignment, and merge decisions.
- Work location is assigned by the repo owner or orchestrator. Do not work directly in another agent's active worktree.
- At most two builder agents may be active at the same time.
- Every builder and reviewer must wake the orchestrator thread when finished by sending a message to that thread.
- Do not start adjacent or later phases unless the orchestrator assigned them.
- If a task is blocked, report the blocker to the orchestrator instead of widening scope.

## Required Reading

Before coding or reviewing, read:

- `docs/SVP_Implementation_Phases_RC1/00_START_HERE.md`
- `docs/SVP_Implementation_Phases_RC1/01_PARALLEL_WORK_MAP.md`
- `docs/SVP_Implementation_Phases_RC1/03_TECHNICAL_DECISIONS.md`
- `docs/SVP_Implementation_Phases_RC1/agent_rules/AGENT_RULES.md`
- `docs/SVP_Implementation_Phases_RC2_Update/00_PAUSE_PHASE_03_PLUS.md`
- The specific phase, track, or review instructions assigned by the orchestrator.

Read only what is needed for the assignment. Do not wander into unrelated phases.

## Git Permissions

Agents assigned implementation or review work are allowed to:

- Create a new branch.
- Create a new worktree when the repo owner or orchestrator assigns one.
- Commit their own completed changes.
- Push their branch.
- Open a pull request when assigned to do so.

These permissions apply only inside the assigned branch/worktree and only for the assigned scope.

Use branch names with the `codex/` prefix unless the orchestrator gives a different name.

Never run destructive git commands such as `git reset --hard`, `git restore`, or checkout commands that discard work unless the orchestrator explicitly approves that exact action.

Before committing:

- Review the full diff.
- Stage only files changed for the assigned task.
- Do not include unrelated user or agent changes.
- Write a commit message based only on verified changes.

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

## Builder Agents

Builder agents must:

- Stay inside the assigned phase.
- Keep changes small and reviewable.
- Build before claiming a code phase is complete.
- Use validator-first discipline. Do not claim a generated `.svp` package is valid unless the validator proves it.
- Report files created, files modified, commands run, tests passed, tests failed, known issues, and the next recommended task.

## Reviewer Agents

Reviewer agents must:

- Review the pull request for correctness, scope control, architecture, and missing proof.
- Prefer code review over rewriting.
- Run commands only when needed to prove or disprove something.
- Leave clear findings with file and line references when possible.
- Wake the orchestrator thread when the review is complete.

If the PR is acceptable, say so plainly. If it needs changes, list the required fixes and do not expand the project scope.

## SVP Project Priorities

- SVP v1.0 RC2 is the active implementation target.
- The validator path is the spine of the project.
- The full builder comes after validator and fixtures are useful.
- The spec is the source of truth.
- Do not silently change RC2 semantics. Document true spec issues for later review.
- Normal `svp build` operation must not require Python.
- Do not continue Phase 03+, fixture lab, media ingest, vision pipeline, relationships/index packaging, or first-video-trial work until OCR and structured color observations are represented in the validator, fixtures, builder roadmap, index/query model, and first-trial acceptance criteria.
- Visible text, numeric values extracted from visible text, and measured color coverage are Core observations in RC2. Do not treat them as labels.

## Handoff Format

When finished, send the orchestrator:

```text
Agent:
Role: builder / reviewer
Branch:
Worktree:
PR:
Assigned scope:
Status: complete / blocked / partial
Files created:
Files modified:
Commands run:
Tests passed:
Tests failed:
Known issues:
Next recommended task:
Wake-up reason:
```
