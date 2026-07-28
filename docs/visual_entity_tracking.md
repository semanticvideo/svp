# Visual Entity Tracking

SVP visual entity tracking uses a bounded 5 Hz RGB sampling plan, five-second
decode windows with one-second overlap, RF-DETR Nano object proposals, motion
and depth support, appearance embeddings, and conservative cross-window
identity reconciliation. Detector categories are internal supporting evidence;
the pipeline does not require or emit user-facing object labels.

Production package builds stream finalized regions and masks to staging as each
window closes. Only the next-window overlap, sixteen compact identity-evidence
regions per entity, and entity/track aggregates remain in memory. The temporary
artifact indexes are filtered at completion so short-lived proposals suppressed
by the persistence policy do not become package records.

## Detector model and license

The reference detector is the Apache-2.0 RF-DETR Nano model published by
Roboflow and exported to ONNX by the ONNX Community:

- Model ID: `model_rfdetr_nano_coco`
- Bundle ID:
  `model_rfdetr_nano_coco@eae21ce-fp32-svp-wrapper-1+blake3_d443faa85b43`
- Upstream model: `onnx-community/rfdetr_nano-ONNX`
- Pinned revision: `eae21cee0687a91bcf9fa071605c48d7705d2d91`
- Upstream FP32 ONNX SHA-256:
  `9cbac6b11ce34a03034e4d5a24cfac5f18632fd6761d1311dd640232088d7fee`
- SVP wrapped ONNX BLAKE3:
  `d443faa85b432a9701aabcee7bb45cf8ded08bacfb983cc4a8dae667767a1300`
- License: Apache-2.0

`spec/registries/reference-model-set.json` pins the bundle identity plus the
path, role, and BLAKE3 of the model, license, documentation, and notice files.
Normal reference model-set verification therefore fails if the detector is
absent, substituted, or missing its exact distribution materials.

The SVP bundle does not alter learned weights. Its ONNX wrapper only flattens
and concatenates the original `pred_boxes` and `logits` outputs into one tensor
for the existing cross-platform ONNX Runtime boundary. The bundle must retain
the upstream Apache-2.0 license, model card, and an SVP notice recording both
the pinned source revision and original artifact hash.

Official sources:

- [RF-DETR repository and model license table](https://github.com/roboflow/rf-detr)
- [Pinned ONNX model repository](https://huggingface.co/onnx-community/rfdetr_nano-ONNX/tree/eae21cee0687a91bcf9fa071605c48d7705d2d91)

## Runtime policy

RF-DETR runs through ONNX Runtime using portable FP32 operators. CPU remains
the cross-platform baseline. The model manifest may advertise additional
execution providers, but unsupported providers must fall back through the
existing runtime boundary rather than changing entity semantics.

Detector inference accepts candidates at 0.05 confidence so an established
track can survive difficult backgrounds. Detector-only candidates require
0.15 confidence to discover a new entity. Weaker candidates must have at least
0.50 predicted-box IoU with an existing track; their category estimate is not
used as identity evidence. This separates continuation from discovery and
prevents weak detections from creating junk entities or stealing established
tracks.

Candidates smaller than 0.5% of the canonical raster are treated as unstable
raster noise; candidates larger than 90% are treated as scene-level evidence.
After deterministic duplicate suppression, at most 32 proposals per frame enter
association so crowded frames cannot create unbounded pairwise work. Processor
provenance records all thresholds plus counts removed by confidence, area,
duplicate, and cap filters so any coverage loss remains measurable.

## Diagnostic baselines

The human-authored fixture is
`packages/svp-vision/tests/fixtures/visual_entity_north_star.json`. Diagnostics
use temporal intervals and sparse spatial checkpoints; labels in the source
notes are descriptive only and are not required pipeline output.

On the Apple M4 Max reference machine using the FP32 detector:

| Input | Entity-only runtime | Peak RSS | Mean interval coverage | Mean checkpoint IoU | Repeated identity consistency |
| --- | ---: | ---: | ---: | ---: | ---: |
| `intro.mp4` | 68.21 s | 1.13 GiB | 92.19% | 0.716 | 100% |
| `test-30.mp4` | 102.84 s | 1.14 GiB | 96.78% | 0.702 | 100% |

These measurements use `svp-visual-entity-diagnostic`, not the full builder,
so ASR and OCR do not affect the result. Dynamic motion groups allow one missed
5 Hz proposal when scoring a checkpoint, while still requiring nearby spatial
evidence from the same entity.
