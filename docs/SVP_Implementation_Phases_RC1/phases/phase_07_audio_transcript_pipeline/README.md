# Phase 07 - Audio, VAD, Transcript, Word Timings, and Speaker Segments

## Phase purpose

Implement the audio observation path required for a real SVP package: VAD, transcript, word-level timestamps, and speaker records.

## Prerequisites

- Phase 06 complete.
- Phase 08 model runtime can be in progress, but at least one transcription path must be selected.

## Primary outputs

```text
packages/svp-audio
VAD regions
transcript/transcript.json
transcript/words.jsonl
transcript/speakers.jsonl
provenance records for audio processors
```

## Work items

1. Add `packages/svp-audio`.
2. Implement audio chunking at deterministic boundaries.
3. Integrate VAD using the reference model path or a deterministic audio energy fallback only for debugging. Production path should use the selected reference VAD model.
4. Integrate transcription through whisper.cpp or a bundled native executable path.
5. Emit per-word timestamps in integer microseconds.
6. Emit `speaker_id` for every word.
7. For first trial, support one-speaker mode if diarization is not yet complete. Record this honestly in provenance.
8. Add optional `speaker_candidates` and `speech_overlap` support in data model even if first trial does not produce overlap.
9. Write transcript files into the package.
10. Add validation checks or fixture updates for audio records.

## Required commands

```bash
cmake --build build
./build/tools/svp-builder/svp-builder build samples/dom-30s.mov --out out/dom-30s.audio.svp --stop-after audio
./build/tools/svp-inspector/svp-inspector inspect out/dom-30s.audio.svp
```

## Definition of done

- Audio pipeline emits transcript files.
- Every word has integer microsecond start/end and singular `speaker_id`.
- One-speaker video can produce useful word timestamps.
- Silent video produces valid zero-word transcript behavior.
- Provenance records list processor and model refs.

## Handoff report format

At the end of this phase, report:

```text
Phase: Phase 07 - Audio, VAD, Transcript, Word Timings, and Speaker Segments
Status: complete / blocked / partial
Files created:
Files modified:
Commands run:
Tests passed:
Tests failed:
Known issues:
Next recommended phase:
```
