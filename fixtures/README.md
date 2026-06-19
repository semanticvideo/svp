# SVP Fixtures

This folder will contain small `.svp` packages used to test validators, readers, inspectors, and eventually builders.

Fixtures should be tiny, deterministic, and purpose-built.

Initial fixture targets:

## static-card

A still image or title card encoded as video. Expected to have valid required sections but no meaningful moving visual tracks.

## vertical-video

A 9:16 source used to validate aspect-preserving canonical raster behavior.

## square-video

A 1:1 source used to validate canonical raster behavior.

## silent-video

A source with no speech. Used to validate transcript and VAD behavior when word count is zero.

## single-speaker

A simple spoken clip with one speaker.

## overlapping-speech

A clip with simultaneous speech, used to validate `speaker_candidates` and `speech_overlap`.

## screen-recording

A UI/screen capture style source.

## moving-object

A clip with one simple moving tracked object.

## equivalence

Fixture pairs generated across different execution providers or hardware paths to test `svp validate --equivalent`.
