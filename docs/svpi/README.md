# SVPI Documentation

SVPI stands for Semantic Video Package Interlace. It is the sidecar form for
SVP-compatible semantic observations bound to an external source media file.

Use this folder for SVPI format design, implementation notes, and review
context.

## Main documents

| File | Purpose |
| --- | --- |
| `SVPI_v0.1_Draft_Specification.md` | Current draft specification for `.svpi` layout, binding, validation, provenance, and media hygiene. |
| `Embedded_SVPI_Transport_ISO_BMFF_v1.md` | Production Embedded SVPI Transport profile for one canonical SVPI in a top-level ISO BMFF `uuid` box. |
| `SVPI_SPEC_AGENT_BRIEF.md` | Original drafting brief and design prompt for the SVPI spec. |

## Core idea

```text
SVP  = packaged source media + semantic observations
SVPI = media-bound semantic observations + no packaged primary media
```

An SVPI can be structurally valid and searchable without the source media
present. It becomes bound when a candidate media file verifies against
`media_binding.json`. Recombination into a full `.svp` requires successful
binding verification.

## Required workflows

```bash
svp-builder interlace create video.mov --out video.svpi
svp-builder interlace inspect video.svpi
svp-builder interlace validate video.svpi --media video.mov
svp-builder interlace extract video.svp --out-dir ./extracted
svp-builder interlace recombine video.mov video.svpi --out video.svp
```

Folder-scale workflows:

```bash
svp-builder interlace create-batch ./media --recursive
svp-builder interlace scan ./media --recursive
svp-builder interlace validate-batch ./media --recursive
svp-builder interlace complete-identity-batch ./media --recursive
```

## Media hygiene

SVPI is not a media container. It must not contain:

```text
media/original/
media/audio/original_stream_*.flac
media/audio/analysis_mono_16k.wav
any replayable source-derived audio, video, or muxed media
```

SVPI may contain:

```text
media/audio/waveform.jsonl
media/audio/audio_absence.json
transcript records
word timestamps
speaker segments
text/evidence_crops/*.jpg
readable still evidence
stream hashes and chunk hashes in media_binding.json
indexes and provenance
```

The shared writer/validator policy is implemented in
`packages/svp-package/src/svpi_media_policy.cpp`.
