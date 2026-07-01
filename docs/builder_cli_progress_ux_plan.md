# Builder CLI Progress and Diagnostics UX Plan

This plan defines the implementation path for making `svp-builder` usable and
calm during long `.svp` and `.svpi` builds. The goal is not only a progress bar.
The goal is a shared build-progress contract that works in terminals, CI logs,
Codex runs, batch workflows, and future app surfaces.

## Problem

The current builder output mixes status messages, stage summaries, validator
details, schema errors, and incidental diagnostics. During normal builds this
looks noisy and makes it hard for a user to understand whether work is
progressing, stuck, successful, or failed.

The CLI needs:

- predictable progress for `.svp` and `.svpi` build paths;
- clean success output;
- concise, useful failure output;
- verbose diagnostics when explicitly requested;
- machine-readable progress for automation;
- terminal-friendly rendering without breaking redirected output or CI logs.

## Core Architecture

The build pipeline must emit neutral progress events. The CLI must render those
events.

Do not put terminal UI behavior inside individual pipeline stages. Pipeline
stages should report facts such as stage start, completion, warnings, artifacts,
and failures. The CLI should decide whether those facts become a plain log,
JSONL, a TTY progress display, or no progress output.

Recommended model:

```text
BuildPipeline / interlace operation
  -> BuildProgressEvent
  -> BuildProgressSink
  -> CLI renderer
       - auto
       - plain
       - json
       - none
```

## Design Requirements

- Use one progress event model for `.svp` and `.svpi` workflows.
- Keep the default sink no-op so library callers are not forced to print.
- Keep renderers outside the build stages.
- Make output deterministic in `plain` and `json` modes.
- Use TTY-aware rendering only when output is interactive.
- Fall back to plain output when stdout/stderr is redirected or not a TTY.
- Respect `NO_COLOR`.
- Keep normal success output concise.
- Put detailed schema/validation diagnostics behind `--verbose` unless the
  command fails.
- On failure, show a short summary plus any validation/report/staging paths that
  help the user debug.
- Avoid a heavy terminal UI dependency unless the builder proves the existing
  standard-library approach is inadequate.

## Stage Catalog

Create a single canonical set of progress stage IDs and labels. Do not duplicate
stage labels independently in `.svp`, `.svpi`, and batch code.

Initial stage IDs should cover at least:

```text
media_probe
media_binding
audio_extract
asr
diarization
vision_plan
color
ocr
depth
embeddings
timeline
entities
relationships
index
package_write
svpi_write
validate
extract
recombine
batch_scan
batch_item
identity
```

Not every command must emit every stage. Stage order should be stable for the
current command. Some stages are determinate and can report a fraction or count;
others are indeterminate and should report start/completion only.

## CLI Surface

Add progress controls consistently to build-oriented commands.

Recommended flags:

```text
--progress auto|plain|json|none
--quiet
--verbose
```

Behavior:

- `auto`: default. Use a TTY-friendly display only for interactive terminals;
  otherwise use plain deterministic lines.
- `plain`: line-oriented human-readable progress.
- `json`: JSONL progress events, one event per line.
- `none`: suppress progress events. Fatal errors and final failure messages may
  still print.
- `quiet`: suppress normal progress and print only final success/failure.
- `verbose`: include detailed diagnostics and full validation/schema findings.

If `--quiet` and `--progress` conflict, define and test the precedence. The
recommended rule is that `--quiet` implies `--progress none` unless `--progress
json` is explicitly supplied for automation.

## Phase 1: Progress Architecture

Scope:

- Add public progress event types and a progress sink/callback interface.
- Add the canonical stage catalog.
- Add a no-op sink as the default.
- Thread the sink through `BuildPipelineOptions` and internal context.
- Emit basic `stage_started`, `stage_completed`, `stage_failed`, `warning`, and
  `artifact_written` events from existing `BuildPipeline` stage boundaries.
- Preserve current user-visible output as much as possible in this phase.
- Add focused unit tests for event ordering and default no-op behavior.

Suggested files:

- `tools/svp-builder/include/svp/builder/build_pipeline.hpp`
- `tools/svp-builder/src/build_pipeline_internal.hpp`
- `tools/svp-builder/src/build_pipeline.cpp`
- new `tools/svp-builder/include/svp/builder/build_progress.hpp`
- new `tools/svp-builder/src/build_progress_events.cpp` or similar
- `tools/svp-builder/tests/build_pipeline_tests.cpp`

Acceptance:

- Normal build behavior still works.
- `BuildPipeline` can be run with a test sink that captures ordered events.
- Event IDs and labels come from one stage catalog.
- No terminal rendering is introduced inside pipeline stages.

## Phase 2: CLI Renderers and Flags

Scope:

- Add `--progress auto|plain|json|none`, `--quiet`, and `--verbose` to
  `svp-builder build`.
- Implement plain renderer.
- Implement JSONL renderer.
- Implement minimal TTY-aware auto renderer with plain fallback.
- Respect `NO_COLOR` and non-TTY output.
- Keep renderer code separate from stage code.
- Add tests for renderer formatting and valid JSONL output.

Suggested files:

- `tools/svp-builder/src/main.cpp`
- new `tools/svp-builder/include/svp/builder/progress_renderer.hpp`
- new `tools/svp-builder/src/progress_renderer.cpp`
- `tools/svp-builder/tests/build_pipeline_tests.cpp`

Acceptance:

- `svp-builder build --progress plain ...` emits deterministic stage lines.
- `svp-builder build --progress json ...` emits valid JSONL progress events.
- `svp-builder build --progress none ...` suppresses progress.
- `auto` does not emit carriage-return/ANSI UI when not running in a TTY.
- Existing tests still pass.

## Phase 3: Full SVP/SVPI Command Wiring

Scope:

- Wire the progress system through:
  - `svp-builder build`
  - `svp-builder interlace create`
  - `svp-builder interlace extract`
  - `svp-builder interlace recombine`
  - `svp-builder interlace create-batch`
  - `svp-builder interlace validate-batch`
  - `svp-builder interlace complete-identity`
  - `svp-builder interlace complete-identity-batch`
- Ensure SVPI semantic creation reports the same build pipeline stages used by
  normal `.svp` builds.
- Add interlace-specific stages for media binding, SVPI write, extraction,
  recombination, and validation.
- Add per-item and aggregate batch progress.
- Keep JSON output modes compatible with existing command JSON output. If a
  command already has `--json`, do not mix progress JSONL into the same stream
  without an explicit design. Prefer progress on stderr or disallow conflicting
  combinations with a clear error.

Suggested files:

- `tools/svp-builder/include/svp/builder/interlace.hpp`
- `tools/svp-builder/include/svp/builder/interlace_batch.hpp`
- `tools/svp-builder/src/interlace_create.cpp`
- `tools/svp-builder/src/interlace_extract.cpp`
- `tools/svp-builder/src/interlace_recombine.cpp`
- `tools/svp-builder/src/interlace_validate.cpp`
- `tools/svp-builder/src/interlace_batch.cpp`
- `tools/svp-builder/src/main.cpp`
- `tools/svp-builder/tests/interlace_tests.cpp`
- `tools/svp-builder/tests/interlace_batch_tests.cpp`

Acceptance:

- Normal `.svp` builds and `.svpi` creates share the same pipeline progress
  events.
- Batch commands show per-file progress in plain/auto modes and structured
  events in JSONL mode.
- Existing `--json` command result output remains valid JSON.
- Conflicting JSON result/progress modes are either separated by stream or
  rejected deterministically.

## Phase 4: Diagnostics and Help Polish

Scope:

- Clean normal success output for build and interlace commands.
- Move noisy schema/validation diagnostics behind `--verbose` unless the command
  fails.
- On validation failure, print a concise summary of top findings and the report
  path when one exists.
- Add examples and workflow guidance to CLI help text.
- Update README/tool docs for progress modes and clean output behavior.
- Add tests that success output is calm and failure output remains actionable.

Suggested files:

- `tools/svp-builder/src/build_progress.cpp`
- `tools/svp-builder/src/package_skeleton_stage.cpp`
- `tools/svp-builder/src/main.cpp`
- `README.md`
- `tools/README.md`
- relevant tests under `tools/svp-builder/tests/`

Acceptance:

- Successful builds no longer dump schema/validation chatter by default.
- Failed builds surface enough detail to act without requiring guesswork.
- `--verbose` exposes full diagnostics.
- CLI `--help` includes progress examples and SVP/SVPI workflows.
- README and tool docs match the implemented behavior.

## Testing Strategy

Each phase must run focused tests for its changed surface. Suggested commands:

```bash
cmake --build build --target svp-builder svp-build-pipeline-tests svp-interlace-tests svp-interlace-batch-tests
./build/tools/svp-builder/svp-build-pipeline-tests
./build/tools/svp-builder/svp-interlace-tests
./build/tools/svp-builder/svp-interlace-batch-tests
```

When diagnostics or validation output changes, also run:

```bash
cmake --build build --target svp-svpi-tests svp-diarization-validation-tests
./build/packages/svp-package/svp-svpi-tests
./build/packages/svp-validation/svp-diarization-validation-tests
```

For real-media proof, use available local sample media and report exact commands,
artifact paths, and whether output was `.svp`, `.svpi`, or both.

## Non-Goals

- Do not redesign the semantic pipeline.
- Do not weaken validation.
- Do not hide failures.
- Do not hard-code progress percentages that only fit one sample video.
- Do not require Python for normal `svp build`.
- Do not replace existing JSON command outputs with progress JSON unless the
  stream contract is explicit and tested.

## Builder Handoff Requirements

Each builder handoff must include:

- phase assigned;
- files created and modified;
- commands run;
- tests passed and failed;
- examples of before/after CLI output for at least one relevant command;
- any remaining noisy output and why it remains;
- any JSON/progress stream contract decisions.
