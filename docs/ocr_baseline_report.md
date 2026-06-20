# OCR Baseline Comparison Report

**Video:** /Users/domesposito/Projects/samples/test-30.mp4
**Date:** Sat Jun 20 15:29:07 MST 2026

## Package Validity
- **Validator Result:** Validator passed with SUCCESS (Exit Code 0).
- **Proof:**
  - **Validated Package:** `build/baseline_run/package.svp` (the final validated `.svp` binary package).
  - **Builder Foundation Metadata:** `build/baseline_run/package.json` (describes the run configuration, staging steps, and validation reports).

## Human Baseline Comparison (test-30.mp4)
**Scene 1 OCR Expected:**
- SVP TEST
- $19.99
- Phoenix, AZ
- June 20, 2026

**OCR Quality Comparison:**
- **Hits:** Successfully extracts legitimate values like `$19.99`, `Phoenix, `, `June 20, `, `SVP`.
- **Misses/Noise:** Parts of scene 1 like `TEST` and `AZ` are unreliable or miss-recognized. Other noisy OCR examples such as `Proen IX, Ke`, `Sune 20,` might appear.
- **Noise Reduction:** Strict filtering has been applied to drop pure punctuation noise or text smaller than 3 alphanumeric characters.

## Feature Completeness Gaps
- **Numeric Extraction:** Baseline numeric value `19.99` is correctly extracted from OCR.
- **Embedding Source:** Spatial embeddings are currently source-derived directly from OCR text observations.
- **ASR/Transcript Gap:** ASR/transcripts are completely absent; they are not faked.
- **Scene/Timeline Gap:** Scene segmentation and timeline boundaries are still foundation-level.
- **Color Limitations:** Color coverage and quantization are at broad/foundation limits without scene-aware refinements.

## Extracted Text Observations

| Raw Text | Normalized Text | Confidence |
|----------|-----------------|------------|
| $19. 99 | $19.99 | 0.675 |
| Proen IX, Ke | proen ix,ke | 0.7766666666666667 |
| S ieee | s ieee | 0.52 |
| Sune 20, | sune 20, | 0.515 |
| SVP | svp | 0.51 |
| Ey a, | ey a, | 0.315 |
| fly i oan = | fly i oan = | 0.4925 |
| mae ~ | mae ~ | 0.645 |
| Piece: | piece: | 0.42 |
| Pr RN = wu | pr rn = wu | 0.5225 |
| Sf Ry | sf ry | 0.39 |
| Sip | sip | 0.39 |
| We, AP | we,ap | 0.53 |
| ae i) | ae i) | 0.445 |
| wnt | wnt | 0.4 |
| N aN a | n an a | 0.38 |
| New » NN Ag | new » nn ag | 0.5325 |
