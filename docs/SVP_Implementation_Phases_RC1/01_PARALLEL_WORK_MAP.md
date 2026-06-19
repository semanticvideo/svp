# Parallel Work Map

This document shows which work can happen in parallel and which work must be stacked.

## RC2 active gate

SVP v1.0 RC2 is the active implementation target. RC2 adds first-class OCR / visible-text observations and structured color observations to Core.

Do not continue Phase 03+, fixture lab, media ingest, vision pipeline, relationships/index packaging, or first-video-trial work until OCR and structured color observations are represented in:

- validator required-path checks
- validator schema checks
- validation-code mapping
- fixture plan
- index/query plan
- builder roadmap
- first-video-trial acceptance criteria

## Hard dependency chain

```text
phase_00_repo_baseline
  -> phase_01_build_system_cli
      -> phase_02_validator_core
          -> RC2 OCR/color plan update
          -> phase_03_validator_deep_package_checks
              -> phase_04_fixture_lab
                  -> phase_05_reader_inspector
                      -> phase_11_first_video_trial
```

The validator path is the spine. It should not be skipped.

## Builder dependency chain

```text
phase_01_build_system_cli
  -> phase_06_builder_media_ingest
      -> phase_07_audio_transcript_pipeline
      -> phase_09_vision_observation_pipeline_with_ocr_and_color
          -> phase_10_index_relationships_packaging
              -> phase_11_first_video_trial
```

Builder work can start once the build system and basic shared libraries exist, but it should not claim package validity until the validator is running.

## Model runtime dependency chain

```text
phase_01_build_system_cli
  -> phase_08_model_bundle_runtime
      -> phase_07_audio_transcript_pipeline
      -> phase_09_vision_observation_pipeline
      -> phase_10_index_relationships_packaging
```

Model runtime can be worked on in parallel with validator work after Phase 1.

## Work that can run in parallel

After Phase 1 is done, these can run concurrently:

```text
Track A: Validator
- phase_02_validator_core
- RC2 OCR/color validator plan update
- phase_03_validator_deep_package_checks after RC2 unblock

Track B: Fixtures
- phase_04_fixture_lab, once Phase 2 has a report format and RC2 OCR/color fixture plan is updated

Track C: Builder media foundation
- phase_06_builder_media_ingest, after RC2 OCR/color builder roadmap is updated

Track D: Model runtime
- phase_08_model_bundle_runtime
```

After Phase 6 and Phase 8 are done, these can run concurrently:

```text
phase_07_audio_transcript_pipeline
phase_09_vision_observation_pipeline_with_ocr_and_color
```

After Phase 7 and Phase 9 are done, run:

```text
phase_10_index_relationships_packaging
phase_11_first_video_trial
```

## Do not parallelize these without coordination

Do not let multiple agents change these at the same time:

```text
CMakeLists.txt
vcpkg.json
packages/svp-core
packages/svp-validation
spec/registries/validation-codes.json
spec/registries/color-buckets.json
spec/registries/color-spaces.json
spec/registries/ocr-observation-types.json
```

These are shared contract zones. Merge conflicts here can become semantic conflicts.

## Suggested agent assignment

```text
Agent 1: Build system + CLI + core libraries
Agent 2: Validator core + RC2 OCR/color deep package validation
Agent 3: Fixtures lab + synthetic SVP packages with OCR/color cases
Agent 4: Model bundle runtime
Agent 5: Builder media ingest + audio pipeline
Agent 6: Vision, OCR, and structured color pipeline
Agent 7: Index, relationships, package writer, first video trial with text/color acceptance
```

One orchestrator should review all PRs or commits.
