# Start Here - SVP RC2 Implementation Plan

## Current repo status

The repository has already been initialized with the SVP v1.0 RC1 release package and updated with the SVP v1.0 RC2 release package.

RC2 is now the active implementation target. RC1 remains in the repo as review history.

Expected repo root:

```text
/Users/domesposito/Projects/svp
```

Expected setup verification command:

```bash
scripts/verify-spec-files.sh
```

That command should pass before any implementation begins or resumes.

## RC2 pause rule

Do not continue Phase 03+, fixture lab, media ingest, vision pipeline, relationships/index packaging, or first-video-trial work until OCR and structured color observations are represented in the implementation plan.

RC2 makes these Core package sections:

```text
text/
  text_regions.jsonl
  text_observations.jsonl
  numeric_values.jsonl
  text_absence.json

colors/
  color_observations.jsonl
  color_summary.json
  color_absence.json
```

Visible text and measured color distribution are observations, not labels. Example: visible text `"$12.99"` is an observation, numeric value `12.99` is an observation, and shot color coverage `orange = 0.70` is an observation.

## What happens next

The original phase plan got the project to a real 30-second video processing trial with valid `/text/` and `/colors/` Core sections. That milestone has now been reached in the implementation: real video packages can be validator-clean with OCR text, numeric values, color observations, depth, OCR evidence crops, OCR-derived embeddings, and ONNX Whisper ASR transcript words when the local model cache and native tools are available.

The next work is completion and hardening. It is still organized through the same phases, but the active backlog is now stackable lanes:

1. Query and inspection usability, including `svp query`.
2. Speaker diarization and speaker embeddings.
3. Visual tracks, spatial regions, and mask blocks.
4. Relationship graph completion.
5. Transcript chunk embeddings and vision embeddings.
6. Strict validator/spec enforcement.
7. Canonical model-bundle BLAKE3 verification.
8. OCR, scene, color, and ASR quality hardening.
9. Installable SVP agent skill and developer guide.

The phases are not all sequential. Some can run in parallel once dependencies and shared contract zones are clear.

## Critical sequencing rule

Do not start with the full video builder.

The correct order is:

1. Commit the RC1 repo baseline.
2. Add native build system and CLI skeleton.
3. Build validator core.
4. Import RC2 and update OCR/color implementation plans.
5. Build deeper package checks with OCR/color schemas, registries, and paths.
6. Create fixtures covering visible text, numeric text, and color coverage.
7. Build reader and inspector with text/color summaries.
8. Build media ingest.
9. Build audio/transcript.
10. Build model runtime.
11. Build vision, OCR, and structured color observations.
12. Build index/relationships/package writer with text/color query tables.
13. Run the first 30-second video trial.

The builder needs the validator. The validator needs fixtures. The fixtures need package libraries. This is how the project avoids building blind.

## What agents should read

Every agent should read:

```text
00_START_HERE.md
01_PARALLEL_WORK_MAP.md
03_TECHNICAL_DECISIONS.md
agent_rules/AGENT_RULES.md
../SVP_Implementation_Phases_RC2_Update/00_PAUSE_PHASE_03_PLUS.md
```

Then the agent should read only the phase doc assigned to it.

## Phase list

```text
phase_00_repo_baseline
phase_01_build_system_cli
phase_02_validator_core
phase_03_validator_deep_package_checks
phase_04_fixture_lab
phase_05_reader_inspector
phase_06_builder_media_ingest
phase_07_audio_transcript_pipeline
phase_08_model_bundle_runtime
phase_09_vision_observation_pipeline
phase_10_index_relationships_packaging
phase_11_first_video_trial
phase_12_hardening_rc_feedback
```

## Definition of an acceptable phase

A phase is done only when:

- Code builds or docs are written at the required paths.
- Commands listed in the phase doc were run.
- Outputs were inspected.
- The agent reports exactly what changed.
- The agent does not start later phases unless explicitly assigned.
