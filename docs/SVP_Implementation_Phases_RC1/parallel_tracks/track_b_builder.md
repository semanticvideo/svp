# Track B - Builder

## Owns

```text
phase_06_builder_media_ingest
phase_07_audio_transcript_pipeline
phase_09_vision_observation_pipeline
phase_10_index_relationships_packaging
phase_11_first_video_trial
```

## Goal

Create the pipeline that turns source media into a complete `.svp` package.

## Core warning

The builder must use the validator. Do not claim success just because a ZIP file was written.

## Stop condition

A 20 to 30 second input video produces an `.svp` package that the validator can inspect and report on.
