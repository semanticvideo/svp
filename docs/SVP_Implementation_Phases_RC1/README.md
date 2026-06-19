# SVP Implementation Phases - RC1 to First Video Package

This package is a repo-ready implementation plan for taking the existing SVP v1.0 RC1 repository from "spec organized" to "I can run a 30-second video through a builder and get a `.svp` package on the other side."

It is written for agents working inside the repository at:

```text
/Users/domesposito/Projects/svp
```

The RC1 repo foundation is already complete. The next work is real implementation work, not more spec drafting.

## What this package contains

```text
SVP_Implementation_Phases_RC1/
├── README.md
├── 00_START_HERE.md
├── 01_PARALLEL_WORK_MAP.md
├── 02_FIRST_30_SECOND_VIDEO_TARGET.md
├── 03_TECHNICAL_DECISIONS.md
├── agent_rules/
│   └── AGENT_RULES.md
├── phases/
│   ├── phase_00_repo_baseline/
│   ├── phase_01_build_system_cli/
│   ├── phase_02_validator_core/
│   ├── phase_03_validator_deep_package_checks/
│   ├── phase_04_fixture_lab/
│   ├── phase_05_reader_inspector/
│   ├── phase_06_builder_media_ingest/
│   ├── phase_07_audio_transcript_pipeline/
│   ├── phase_08_model_bundle_runtime/
│   ├── phase_09_vision_observation_pipeline/
│   ├── phase_10_index_relationships_packaging/
│   ├── phase_11_first_video_trial/
│   └── phase_12_hardening_rc_feedback/
├── parallel_tracks/
├── checklists/
└── agent_prompts/
```

## The one-sentence strategy

Build the validator and package infrastructure first, then build the reader/inspector, then build the media/audio/vision pipelines, then integrate the full builder and run a 30-second video through it.

## The milestone that matters

The target milestone is:

```bash
svp build samples/dom-30s.mov --out out/dom-30s.svp
svp validate out/dom-30s.svp --json
svp inspect out/dom-30s.svp
```

The resulting `.svp` does not have to be perfect yet, but it must be structurally valid, inspectable, and produced by the actual pipeline rather than assembled by hand.
