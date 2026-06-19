# Start Here - SVP RC1 Implementation Plan

## Current repo status

The repository has already been initialized with the SVP v1.0 RC1 release package.

Expected repo root:

```text
/Users/domesposito/Projects/svp
```

Expected setup verification command:

```bash
scripts/verify-spec-files.sh
```

That command should pass before any implementation begins.

## What happens next

The next work is split into practical phases that get the project to a real 30-second video processing trial.

The phases are not all sequential. Some can run in parallel once the build system exists.

## Critical sequencing rule

Do not start with the full video builder.

The correct order is:

1. Commit the RC1 repo baseline.
2. Add native build system and CLI skeleton.
3. Build validator core.
4. Build deeper package checks.
5. Create fixtures.
6. Build reader and inspector.
7. Build media ingest.
8. Build audio/transcript.
9. Build model runtime.
10. Build vision observations.
11. Build index/relationships/package writer.
12. Run the first 30-second video trial.

The builder needs the validator. The validator needs fixtures. The fixtures need package libraries. This is how the project avoids building blind.

## What agents should read

Every agent should read:

```text
00_START_HERE.md
01_PARALLEL_WORK_MAP.md
03_TECHNICAL_DECISIONS.md
agent_rules/AGENT_RULES.md
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
