# First 30-Second Video Target

This is the concrete target for the first real end-to-end trial.

## Goal

Process one 30-second video through the SVP builder and produce an RC2 `.svp` package that can be validated and inspected.

The target command should eventually be:

```bash
svp build samples/dom-30s.mov --out out/dom-30s.svp
svp validate out/dom-30s.svp --json
svp inspect out/dom-30s.svp
```

If the CLI is still split into separate binaries during early implementation, this is acceptable:

```bash
svp-builder build samples/dom-30s.mov --out out/dom-30s.svp
svp-validator validate out/dom-30s.svp --json
svp-inspector inspect out/dom-30s.svp
```

## Source video requirements

Use a deliberately simple first test video:

```text
Duration: 20 to 30 seconds
Resolution: 1080p or lower
Orientation: landscape 16:9 for the first trial
Audio: one speaker, clean speech
Visuals: one talking head or one clear object entering frame, preferably with at least one visible text/number element
No heavy edits
No music bed
No overlapping speech
No complex screen share for the first pass
```

After that works, run a second vertical 9:16 test and a screen/UI-style text-heavy test.

## Required package contents for first trial

The generated `.svp` must include all required SVP Core sections from RC2, including:

```text
manifest.json
media/original
media/audio
transcript/transcript.json
transcript/words.jsonl
transcript/speakers.jsonl
text/text_regions.jsonl
text/text_observations.jsonl
text/numeric_values.jsonl
text/text_absence.json
timeline/shots.jsonl
timeline/scenes.jsonl
spatial/entity_tracks.jsonl
spatial/regions.jsonl
spatial/depth.blocks.svpdz
spatial/masks.blocks.svpmz
colors/color_observations.jsonl
colors/color_summary.json
colors/color_absence.json
embeddings/embeddings.blocks.svpez
relationships/relationships.jsonl
index/index.sqlite
index/index_manifest.json
provenance/validation.json
```

If the exact RC2 folder names differ, follow the spec. This document describes intent.

## Acceptable early limitations

For the first video trial, the following are acceptable if documented in `provenance` and validation still passes:

- One primary speaker only.
- Simple speaker diarization.
- Coarse visual entity tracks.
- Conservative masks.
- Depth generated at canonical analysis raster only.
- Embeddings generated only at required chunk granularity.
- OCR may report valid text absence only when the source actually contains no visible text.
- Numeric extraction may be empty when visible text contains no safely parseable numbers.
- Color summaries may be coarse bucket distributions first, but every shot and scene should have queryable color coverage.
- Relationships limited to time overlap, speaking while visible, region overlap, and shot containment.

The package must not fake required sections. Empty sections are acceptable only where the spec allows them and the source actually justifies them.

## RC2 OCR and color acceptance

The first real video trial must prove:

- `/text/` exists and validates.
- `/colors/` exists and validates.
- `svp inspect` reports text region counts and color observation counts.
- At least one scene or shot color summary is queryable.
- Scene/shot color percentages can support queries like "find orange scenes."
- If visible text appears in the video, it appears in text observations and the index.
- If visible text contains a safely parseable number, a numeric value record is produced.

## Non-negotiable acceptance

The trial is successful only when:

```bash
svp validate out/dom-30s.svp --json
```

returns a structured RC2 report and does not crash.

The ideal target is `valid` or `valid_with_warnings`. If it returns `invalid`, the invalid findings must be specific, registry-backed, and actionable.
