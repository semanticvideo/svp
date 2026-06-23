# SVP Implementation Phases - RC2 to First Video Package

This package is a repo-ready implementation plan for taking the existing SVP repository from "spec organized" to "I can run a 30-second video through a builder and get a `.svp` package on the other side."

It is written for agents working inside the repository at:

```text
/Users/domesposito/Projects/svp
```

The RC1 repo foundation is complete, and SVP v1.0 RC2 is now the active implementation target. RC2 adds first-class OCR / visible-text observations and structured color observations to Core. The original first-video-package milestone has been reached; current work is now RC2/reference-completion hardening over real builder output.

The folder name still says `RC1` because this package began as the RC1 implementation plan. Active RC2 deltas live in:

```text
docs/SVP_Implementation_Phases_RC2_Update/
```

The RC2 pause gate has been satisfied. Continue using the RC2 deltas as the active contract for validator, builder, index/query, package, and inspection work.

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

Preserve the validator-clean real builder path while filling the remaining RC2 semantic layers, tightening validator coverage, and hardening quality based on real-video trials.

## The milestone that matters now

The original target milestone was:

```bash
svp build samples/dom-30s.mov --out out/dom-30s.svp
svp validate out/dom-30s.svp --json
svp inspect out/dom-30s.svp
```

That milestone is now met in principle by the actual pipeline: real sample videos can build validator-clean packages with transcript, OCR, color, timeline, relationship, depth, embedding, index, provenance, and validation artifacts when local tools and model bundles are present.

The current milestone is completion and hardening:

- Harden visual entity identity with deterministic fixtures for crossing paths, occlusion, stop/resume behavior, and depth-only separation.
- Add relationship traversal queries.
- Add mask-based spatial relationships such as overlaps, contains, and occludes.
- Tighten validator coverage for diarization fallback, relationships, model identity, visual entity artifacts, and package hygiene.
- Improve OCR spacing/evidence verification, ASR confidence/alignment, scene/timeline quality, and model-bundle conformance.
