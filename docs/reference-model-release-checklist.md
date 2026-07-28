# Reference model release checklist

This checklist covers the eight production models in
`spec/registries/reference-model-set.json`. Registry entries are the shipping
identities. `distribution/reference-models/catalog.json` owns download
provenance for the future native installer.

The installer reconstructs the approved set directly from immutable upstream
sources. It verifies the catalog's expected byte count, SHA-256, and BLAKE3 for
every download, runs the pinned RF-DETR and PP-OCR conversion recipes, and then
verifies each reconstructed output. No mirror or prebuilt bundle archive is a
required installation path.

Bundles contain `model.svpmodel.json`, their exact runtime files, `LICENSE`, and
`NOTICE`. One authoritative `model-lock.json` lives at the staged cache root;
it identifies all eight bundle versions and aggregate digests and repeats every
required file path, role, and BLAKE3. The lock is never copied into bundles.

## Release procedure

1. Confirm that the registry still contains exactly the model IDs used by the
   production processors.
2. For every catalog artifact, confirm the immutable revision or release asset
   identity, remote SHA-256, local BLAKE3, license, and notice obligations.
3. Reproduce RF-DETR with `scripts/reproduce-reference-rfdetr-model.py` under
   the exact Python, ONNX, protobuf, NumPy, ml-dtypes, and typing-extensions
   versions recorded in the catalog and installable from
   `scripts/requirements-rfdetr-reproduction.txt`.
4. Reproduce both PP-OCR models with
   `scripts/reproduce-reference-ppocr-models.py` under the exact toolchain in
   the catalog. The Paddle2ONNX command must omit `--optimize_tool` so the
   required default Polygraphy folding runs.
5. Run `scripts/prepare-reference-model-bundles.py` against an artifacts-only
   source cache, reproduced artifacts, and pinned upstream legal files in a new
   staging directory. Canonical manifest metadata, file roles, and NOTICE text
   come only from the committed `distribution/reference-models/bundle-inputs.json`.
6. Verify every generated directory with `svp-models-tool verify --bundle-dir`.
7. Verify the root lock with `svp-models-tool verify --lock
   <staging-dir>/model-lock.json --cache-dir <staging-dir>`.
8. Verify the complete output with `svp-models-tool verify --model-set
   spec/registries/reference-model-set.json --cache-dir <staging-dir>`.
9. Confirm that every staged runtime artifact is byte-for-byte identical to the
   production cache identity recorded in the catalog. Do not substitute models.
10. Publish the complete staged cache transactionally only after every prior
    check passes.

## RC2 public artifact procedure

RC1 is preserved as historical release material and MUST NOT be regenerated.
After changing the active RC2 Markdown or its packaged normative files, create
an isolated release-tool environment and regenerate every public RC2 artifact
with one command:

```bash
python3.11 -m venv /tmp/svp-rc2-artifacts-venv
/tmp/svp-rc2-artifacts-venv/bin/pip install -r scripts/requirements-rc2-artifacts.txt
/tmp/svp-rc2-artifacts-venv/bin/python scripts/generate-rc2-release-artifacts.py
```

The generator renders `spec/SVP_v1_0_RC2.md` into the tracked DOCX and PDF,
semantically checks all three forms for the current model-lock contract, and
rebuilds `releases/SVP_v1_0_RC2_Release_Package.zip` with fixed ZIP metadata and
the active RC2 companion, registries, schemas, review notes, and implementation
update documents.

## Proven artifact inventory

| Model | Source identity | License material | Status |
|---|---|---|---|
| Whisper Small English | `ggerganov/whisper.cpp@c521a4b02f422512d734391fdf08bb08c0862f68`, `ggml-small.en.bin` | OpenAI MIT license at `openai/whisper@6e3be77e1a105e59086e3e21ff5f609fd6fa89a5` | Ready for direct download and local assembly |
| sherpa-onnx diarization | pyannote conversion `@9403a6902bb58e3d5ae8c7e77c3422de279db2e0`; 3D-Speaker GitHub release asset `198893098` through its numeric asset API | pyannote MIT plus 3D-Speaker Apache-2.0, both preserved in the combined bundle license and notice | Ready for direct download and local assembly |
| Depth Anything V2 Small | `onnx-community/depth-anything-v2-small@f7421df0cc30f121782ab050d42f0a423291dcde`, `onnx/model.onnx` | Apache-2.0 | Ready for direct download and local assembly |
| Nomic Embed Text v1.5 | `nomic-ai/nomic-embed-text-v1.5@ac6fcd72429d86ff25c17895e47a9bfcfc50c1b2` | Apache-2.0 | Ready for direct download and local assembly |
| Nomic Embed Vision v1.5 | `nomic-ai/nomic-embed-vision-v1.5@e3a725bce72db07ca4adb1d83da08903f3ee02f8` | Apache-2.0 | Ready for direct download and local assembly |
| RF-DETR Nano COCO | upstream `onnx-community/rfdetr_nano-ONNX@eae21cee0687a91bcf9fa071605c48d7705d2d91`; deterministic FP32 wrapper revision `2` | pinned Roboflow Apache-2.0 license plus upstream model card and attribution | Ready for deterministic reconstruction |
| PP-OCRv6 medium detector | `PaddlePaddle/PP-OCRv6_medium_det@8e0f56fb2ef86b461d99cfc7ac5c137738985f61`; pinned Paddle2ONNX recipe | pinned PaddleOCR Apache-2.0 license and generated provenance notice | Ready for deterministic reconstruction |
| PP-OCRv6 medium recognizer | `PaddlePaddle/PP-OCRv6_medium_rec@e5a92bcbc5cc1b494628e458d267778f0704fd7c`; pinned Paddle2ONNX recipe plus immutable `inference.yml` | pinned PaddleOCR Apache-2.0 license and generated provenance notice | Ready for deterministic reconstruction |

The approved set contains only the runtime artifacts named above. Quantized,
alternate-precision, and otherwise unused files from upstream repositories or
development caches are outside the release set.
