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

## Current Scope

The current bake-off scope is to improve speaker attribution while preserving
the product shape of a quiet local background builder.

Required behavior:

- Preserve the current good speaker-count behavior.
- Improve the two-speaker real sample attribution to at least 95% against the
  Premiere CSV reference, ideally closer to 100%.
- Keep the similar-timbre two-speaker fixture at 100% word-level attribution
  against its Premiere reference.
- Keep all named sample counts and existing fixture expectations stable.
- Do not use the rejected model-swap contestants as the production path.
- Do not run a hidden second speaker-embedding/refinement phase after the
  diarization progress has completed.
- Any speaker attribution refinement that is part of diarization must happen
  before the existing diarization stage is marked complete. The progress UI
  does not need a redesign; the existing 0-100% stage must simply account for
  all diarization work.
- Do not add a surprise fallback path. If refinement cannot run, produce honest
  segment-overlap assignments and record the limitation rather than doing
  unaccounted work later.
- Keep normal `svp build` local-only, cross-platform viable, and free of a
  Python runtime requirement.
- Keep concerns separated; avoid adding more responsibility to already-large
  files when a separate decoder/refinement owner is clearer.

Memory acceptance framing:

- There is no hard 2 GB cap.
- Lower and predictable memory is strongly preferred because the product should
  run quietly in the background.
- A higher footprint is acceptable only when it buys meaningful quality and does
  not create avoidable late-stage spikes.
- Paths around a few GB are directionally acceptable if quality is strong.
- Paths around 7-9 GB for equal or worse quality are not acceptable.
- Avoid duplicate model/extractor passes and avoid rereading the full audio for
  hidden late speaker refinement.

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

## Contestant Results

The first model-swap contestants were tested without production code changes by
using diagnostic model-cache roots that keep the existing sherpa segmentation
model and place each candidate speaker embedding model at the filename expected
by the current implementation.

| Contestant | Model cache entry | MONIQUE speaker count | MONIQUE attribution | Result |
| --- | --- | ---: | ---: | --- |
| Current baseline | `/Users/domesposito/Projects/svp-model-cache/model_sherpa_onnx_diarization` | 2 | 94.2925% | Baseline to beat. |
| 3D-Speaker CAM++ VoxCeleb | `/Users/domesposito/Projects/svp-model-cache/model_3dspeaker_campplus_sv_en_voxceleb_16k` | 8 | 71.7004% best-effort majority map | Fails count gate and attribution. |
| 3D-Speaker ERes2NetV2 zh-cn common | `/Users/domesposito/Projects/svp-model-cache/model_3dspeaker_eres2netv2_sv_zh_cn_16k_common` | 5 | 94.2331% best-effort majority map | Fails count gate and is slightly worse than baseline. |
| WeSpeaker CAM++ VoxCeleb | `/Users/domesposito/Projects/svp-model-cache/model_wespeaker_campp_sv_en_voxceleb_16k` | 5 | 71.7004% best-effort majority map | Fails count gate and attribution. |

Diagnostic output roots:

| Contestant | Output root |
| --- | --- |
| 3D-Speaker CAM++ VoxCeleb | `/Users/domesposito/Projects/svp/build/diagnostics/diarization-bakeoff/campplus-monique` |
| 3D-Speaker ERes2NetV2 zh-cn common | `/Users/domesposito/Projects/svp/build/diagnostics/diarization-bakeoff/eres2netv2-monique` |
| WeSpeaker CAM++ VoxCeleb | `/Users/domesposito/Projects/svp/build/diagnostics/diarization-bakeoff/wespeaker-campp-monique` |

Initial conclusion: direct embedding model swaps do not beat the current
baseline under the existing reconciliation and word-refinement logic. The next
contestant should be a decoder/assignment change over the current embedding
path, not another blind model swap.

Observed memory notes from manual Activity Monitor/watch during the initial
model-swap runs:

| Contestant | Observed peak footprint | Note |
| --- | ---: | --- |
| Current baseline | Within desired current range | Baseline branch behavior remains the memory target. |
| 3D-Speaker CAM++ VoxCeleb | Roughly 4-5 GB | Higher than desired and did not beat baseline quality. |
| 3D-Speaker ERes2NetV2 zh-cn common | Roughly 8-9 GB | Too high for the target environment and did not beat baseline quality. |
| WeSpeaker CAM++ VoxCeleb | Roughly 3 GB | Lower than the 3D-Speaker swaps but still failed count and attribution gates. |

These memory observations were not captured by automated diagnostics. If a
future contestant looks promising on quality, rerun it with memory diagnostics
enabled before considering it production-worthy.

## Current Checkpoint: Segment-Lock Plus One-Speaker Gap Fix

Recorded on branch:

```text
codex/fix-two-speaker-diarization
```

Implementation shape at this checkpoint:

- Keeps selective word-local fingerprinting for unstable words and boundaries.
- Aggregates clear segment overlap by speaker before using it as a lock.
- Locks strong direct segment evidence in 3+ speaker media so later
  fingerprint/decoder smoothing cannot collapse distinct speakers when the
  diarization segments are already decisive.
- Treats real one-speaker diarization as one speaker even when ASR words fall
  outside segment coverage, instead of creating `speaker_unknown` as a second
  transcript speaker.

Measured current results:

| Case | Expected | Current result | Quality / count | Memory measured |
| --- | ---: | ---: | --- | ---: |
| Real ASR fixture: one speaker | 1 | Pass | Real ASR + diarization attribution test passes. | Not separately logged |
| Real ASR fixture: two speakers | 2 | Pass | Real ASR + diarization attribution test passes. | Not separately logged |
| Real ASR fixture: three speakers | 3 | Pass | Real ASR + diarization attribution test passes. | Not separately logged |
| Similar-timbre fixture | 2 | 2 | Speaker-side attribution remains 52/52 correct. | 0.576 GB peak RSS / 0.555 GB footprint |
| MONIQUE.mp4 | 2 | 2 | 95.8509% against `MONIQUE.csv` quick comparison; 1594 correct, 69 wrong, 20 unmapped by strict CSV span. | 7.687 GB peak RSS / 7.571 GB footprint |
| new-gator.mp4 | Roughly 17; 20 accepted | 20 | Count remains acceptable; no per-speaker reference yet. | 8.700 GB peak RSS / 85.717 GB footprint |
| intro.mp4 | 1 | 1 | 51/51 words assigned to `speaker_0001`. | Not captured |
| test-30.mp4 | 1 | 1 | 58/58 words assigned to `speaker_0001`. | Not captured |

Fresh diagnostic output roots:

| Case | Output root |
| --- | --- |
| MONIQUE replay | `/Users/domesposito/Projects/svp/build/diagnostics/current-segment-lock-monique-v1` |
| Similar-timbre replay | `/Users/domesposito/Projects/svp/build/diagnostics/current-segment-lock-similar-v1` |
| Gator replay | `/Users/domesposito/Projects/svp/build/diagnostics/current-segment-lock-gator-v1` |
| intro automatic audio stage | `/Users/domesposito/Projects/svp/build/diagnostics/current-single-speaker-intro-v2` |
| test-30 automatic audio stage | `/Users/domesposito/Projects/svp/build/diagnostics/current-single-speaker-test30-v2` |

Commands run for this checkpoint:

```text
SVP_SHERPA_DIARIZATION_MODEL_DIR=/Users/domesposito/Projects/svp-model-cache/model_sherpa_onnx_diarization \
  ctest --test-dir build --output-on-failure -R svp-audio-tests

SVP_MODEL_CACHE_DIR=/Users/domesposito/Projects/svp-model-cache \
SVP_SHERPA_DIARIZATION_MODEL_DIR=/Users/domesposito/Projects/svp-model-cache/model_sherpa_onnx_diarization \
  ctest --test-dir build --output-on-failure -R svp-audio-tests
```

Both test runs passed. The real-model run exercises the one/two/three fixture
attribution path using real fixture WAVs, real Whisper ASR, and real Sherpa
diarization.

Difference from the recorded baseline:

| Metric | Baseline | Current checkpoint | Change |
| --- | ---: | ---: | ---: |
| MONIQUE attribution | 94.2925% | 95.8509% | +1.5584 percentage points |
| MONIQUE correct words | 1586 | 1594 | +8 |
| MONIQUE wrong words | 96 | 69 | -27 |
| MONIQUE Speaker 1 recall | 95.19% | 97.57% | +2.38 points |
| MONIQUE Speaker 2 recall | 92.02% | 91.45% | -0.57 points |
| MONIQUE Speaker 2 precision | 88.31% | 93.65% | +5.34 points |
| Similar-timbre attribution | 52/52 | 52/52 | No regression |
| Gator accepted speaker count | 20 | 20 | No regression |

Current caveats:

- The MONIQUE percentage uses the current quick CSV comparison. It is useful
  for branch-to-branch comparison, but the 20 unmapped words are strict CSV-span
  misses and should not be treated as speaker-attribution errors without
  reviewing their boundaries.
- Gator footprint remains high in the diagnostic replay even though peak RSS is
  lower than the earlier 12.33 GB replay measurement.
- intro.mp4 and test-30.mp4 were corrected after this checkpoint exposed that
  one-speaker segment gaps could create `speaker_unknown` as a second transcript
  speaker.

## Distribution Constraints

Any winning candidate must be:

- local-only for normal `svp build`;
- viable across macOS, Windows, and Linux;
- usable from the C++/ONNX runtime path without requiring Python at runtime;
- packageable as an SVP model bundle with exact revision, license text, notice
  text, and file hashes;
- scored with the same reference inputs and scorer used for the other
  contestants.
