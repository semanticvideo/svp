# First 30-Second Video Target

This is the concrete target for the first real end-to-end trial.

## Goal

Process one 30-second video through the SVP builder and produce a `.svp` package that can be validated and inspected.

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
Visuals: one talking head or one clear object entering frame
No heavy edits
No music bed
No overlapping speech
No screen share for the first pass
```

After that works, run a second vertical 9:16 test.

## Required package contents for first trial

The generated `.svp` must include all required SVP Core sections from RC1, including:

```text
manifest.json
media/original
media/audio
transcript/transcript.json
transcript/words.jsonl
transcript/speakers.jsonl
timeline/shots.jsonl
timeline/scenes.jsonl
spatial/entity_tracks.jsonl
spatial/regions.jsonl
spatial/depth.blocks.svpdz
spatial/masks.blocks.svpmz
embeddings/embeddings.blocks.svpez
relationships/relationships.jsonl
index/index.sqlite
index/index_manifest.json
provenance/validation.json
```

If the exact RC1 folder names differ, follow the spec. This document describes intent.

## Acceptable early limitations

For the first video trial, the following are acceptable if documented in `provenance` and validation still passes:

- One primary speaker only.
- Simple speaker diarization.
- Coarse visual entity tracks.
- Conservative masks.
- Depth generated at canonical analysis raster only.
- Embeddings generated only at required chunk granularity.
- Relationships limited to time overlap, speaking while visible, region overlap, and shot containment.

The package must not fake required sections. Empty sections are acceptable only where the spec allows them and the source actually justifies them.

## Non-negotiable acceptance

The trial is successful only when:

```bash
svp validate out/dom-30s.svp --json
```

returns a structured report and does not crash.

The ideal target is `valid` or `valid_with_warnings`. If it returns `invalid`, the invalid findings must be specific, registry-backed, and actionable.
