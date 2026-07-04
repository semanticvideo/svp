# Diarization Bake-Off Notes

This document tracks diarization attribution experiments against fixed reference
outputs. The goal is to compare implementation candidates without changing the
scoring target or silently tuning behavior to a single sample.

## Scoring Targets

The bake-off target is 100% word-level speaker attribution where a reference is
available. The minimum acceptable target for the two-speaker real sample is 95%.

Required count expectations:

| Sample | Expected speakers | Notes |
| --- | ---: | --- |
| intro.mp4 | 1 | Must remain stable. |
| MONIQUE.mp4 | 2 | Premiere CSV is the current attribution reference. |
| test-30.mp4 | 1 | Must remain stable. |
| new-gator.mp4 | 20 accepted | Roughly 17 true speakers; current 20 is acceptable. |
| Existing fixtures | Existing expected counts | Must remain stable. |

## Reference Inputs

| Reference | Path | Purpose |
| --- | --- | --- |
| Two-speaker real sample reference | `/Users/domesposito/Projects/samples/Premiere Info/Monique/MONIQUE.csv` | Premiere Pro diarization reference for word attribution scoring. |
| Similar-timbre fixture reference | `/Users/domesposito/Projects/samples/Premiere Info/Monique/speakers.csv` | Premiere Pro reference for the similar-timbre two-speaker fixture. |
| Similar-timbre fixture source | `/Users/domesposito/Projects/svp/fixtures/audio/sherpa-diarization/similar-timbre-two-speaker.wav` | Full audio/ASR/diarization path fixture source. |
| Gator diagnostic audio | `/Users/domesposito/Projects/svp/build/diagnostics/gator-analysis-mono-16k.wav` | Existing extracted diagnostic audio for long gator sample. |

## Current Baseline

Branch at baseline recording time:

```text
codex/fix-two-speaker-diarization
```

Current implementation summary:

- Collapses a dominant speaker plus fragmented secondary tracks into two
  speakers using a general reconciliation policy.
- Adds sustained non-dominant utterance expansion in transcript writing.
- Adds local voice fingerprint refinement using diarization embeddings and
  bounded final speaker IDs.

Latest measured attribution scores:

| Case | Score | Detail |
| --- | ---: | --- |
| Two-speaker real sample | 94.2925% | 1586 correct, 96 wrong, 1 unmapped/no match. |
| Similar-timbre two-speaker fixture | 100% | 52/52 words matched expected attribution. |

Two-speaker real sample confusion summary:

| Reference speaker | Output speaker | Count |
| --- | --- | ---: |
| Speaker 1 | Speaker 1 | 1148 |
| Speaker 1 | Speaker 2 | 58 |
| Speaker 2 | Speaker 2 | 438 |
| Speaker 2 | Speaker 1 | 38 |

Derived recall/precision:

| Metric | Value |
| --- | ---: |
| Speaker 1 recall | 95.19% |
| Speaker 2 recall | 92.02% |
| Speaker 2 precision | 88.31% |

## Bake-Off Candidates

| Candidate | Purpose | Production constraints |
| --- | --- | --- |
| Current embeddings plus sequence decoder | Tests whether decision logic is the main remaining problem. | Must stay local and avoid sample-specific thresholds. |
| 3D-Speaker / CAM++ ONNX fingerprinting | Tests a stronger local speaker embedding path with a clean Apache-2.0-oriented story. | Must be packageable as an SVP model bundle with exact license, notice, revision, and hashes. |
| WeSpeaker ONNX fingerprinting | Tests an alternate ONNX speaker embedding path. | Exact model-weight license and attribution requirements must be recorded before production use. |

## Distribution Constraints

Any winning candidate must be:

- local-only for normal `svp build`;
- viable across macOS, Windows, and Linux;
- usable from the C++/ONNX runtime path without requiring Python at runtime;
- packageable as an SVP model bundle with exact revision, license text, notice
  text, and file hashes;
- scored with the same reference inputs and scorer used for the other
  contestants.

