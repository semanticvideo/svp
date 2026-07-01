# SVP Fixtures

This folder will contain small `.svp` packages used to test validators, readers, inspectors, and eventually builders.

Fixtures should be tiny, deterministic, and purpose-built.

Current fixtures are `.svp` packages unless a fixture folder explicitly says otherwise.
Future `.svpi` sidecar fixtures should prove the interlace contract separately:
no `media/original/`, no replayable source-derived audio/video/muxed media,
allowed OCR crops and other bounded evidence, valid `media_binding.json`, and
recombination back into a full `.svp` package.

## phase-04-ocr-color

Generated RC2 OCR/color fixture packages for the current validator. See
`fixtures/phase-04-ocr-color/README.md` and regenerate with:

```bash
python3 fixtures/tools/make_phase04_ocr_color_fixtures.py
```

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
