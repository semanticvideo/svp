#!/usr/bin/env python3
"""Assemble and verify the eight approved reference-model bundles.

This is release tooling, not an SVP runtime dependency. It never downloads or
uploads weights. The caller supplies the proven upstream-derived artifacts, the
existing runtime metadata, and the native verifier. The output directory is a
complete staging tree with one authoritative root model-lock.json.
"""

import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path


ZERO_HASH = "blake3:" + ("0" * 64)

MODELS = {
    "model_whisper_small_en": {
        "files": [("ggml-small.en.bin", "weights")],
        "notice": """SVP Model Bundle: model_whisper_small_en

Weights: OpenAI Whisper Small English, converted to GGML for whisper.cpp.
Source: https://huggingface.co/ggerganov/whisper.cpp
Pinned revision: c521a4b02f422512d734391fdf08bb08c0862f68
Artifact: ggml-small.en.bin
License: MIT
""",
    },
    "model_sherpa_onnx_diarization": {
        "files": [
            ("sherpa-onnx-pyannote-segmentation-3-0/model.onnx", "segmentation"),
            ("3dspeaker_speech_eres2net_base_sv_zh-cn_3dspeaker_16k.onnx", "embedding"),
        ],
        "notice": """SVP Model Bundle: model_sherpa_onnx_diarization

Segmentation: pyannote segmentation-3.0 ONNX conversion by csukuangfj.
Pinned revision: 9403a6902bb58e3d5ae8c7e77c3422de279db2e0
License: MIT; copyright (c) 2022 CNRS.

Embedding: 3D-Speaker ERes2Net ONNX conversion distributed by sherpa-onnx.
Pinned GitHub release asset ID: 198893098
License: Apache-2.0; upstream project Alibaba DAMO Academy 3D-Speaker.
""",
    },
    "model_depth_anything_v2_small": {
        "files": [("model.onnx", "model")],
        "notice": """SVP Model Bundle: model_depth_anything_v2_small

Source: onnx-community/depth-anything-v2-small
Original project: DepthAnything/Depth-Anything-V2
Pinned revision: f7421df0cc30f121782ab050d42f0a423291dcde
Pinned license revision: 0a7e2b58a7e378c7863bd7486afc659c41f9ef99
Artifact: onnx/model.onnx
License: Apache-2.0
""",
    },
    "model_nomic_embed_text_v1_5": {
        "files": [
            ("model.onnx", "model"),
            ("vocab.txt", "tokenizer_vocab"),
        ],
        "notice": """SVP Model Bundle: model_nomic_embed_text_v1_5

Source: nomic-ai/nomic-embed-text-v1.5
Pinned revision: ac6fcd72429d86ff25c17895e47a9bfcfc50c1b2
Artifacts: onnx/model.onnx plus the runtime vocabulary
License: Apache-2.0, from the pinned nomic-ai/contrastors license source.
""",
    },
    "model_nomic_embed_vision_v1_5": {
        "files": [("model.onnx", "model")],
        "notice": """SVP Model Bundle: model_nomic_embed_vision_v1_5

Source: nomic-ai/nomic-embed-vision-v1.5
Pinned revision: e3a725bce72db07ca4adb1d83da08903f3ee02f8
Artifact: onnx/model.onnx
License: Apache-2.0, from the pinned nomic-ai/contrastors license source.
""",
    },
    "model_rfdetr_nano_coco": {
        "files": [
            ("model.onnx", "model"),
            ("UPSTREAM-MODEL-CARD.md", "documentation"),
        ],
        "notice": """SVP Model Bundle: model_rfdetr_nano_coco

Weights: RF-DETR Nano, Apache-designated model weights from Roboflow.
ONNX source: onnx-community/rfdetr_nano-ONNX
Pinned upstream revision: eae21cee0687a91bcf9fa071605c48d7705d2d91
SVP wrapper revision: 2 (flattened pred_boxes and logits output boundary)
The wrapper metadata identifies the upstream graph as FP32. Learned weights,
nodes, inputs, and inference outputs are unchanged from wrapper revision 1.
License: Apache-2.0, from the pinned roboflow/rf-detr license source.
""",
    },
    "model_pp_ocrv6_medium_det": {
        "files": [("det.onnx", "model")],
        "legacy_manifest": "model_manifest.json",
        "notice": """SVP Model Bundle: model_pp_ocrv6_medium_det

Source model: PaddlePaddle/PP-OCRv6_medium_det.
Pinned revision: 8e0f56fb2ef86b461d99cfc7ac5c137738985f61
Artifact: deterministic Paddle2ONNX 2.1.0 opset-14 export with default
Polygraphy folding from the pinned inference.json and inference.pdiparams.
License: Apache-2.0
""",
    },
    "model_pp_ocrv6_medium_rec": {
        "files": [("rec.onnx", "model"), ("inference.yml", "character_dictionary")],
        "legacy_manifest": "model_manifest.json",
        "notice": """SVP Model Bundle: model_pp_ocrv6_medium_rec

Source model: PaddlePaddle/PP-OCRv6_medium_rec.
Pinned revision: e5a92bcbc5cc1b494628e458d267778f0704fd7c
Artifacts: deterministic Paddle2ONNX 2.1.0 opset-14 export with default
Polygraphy folding, plus the pinned upstream inference.yml character dictionary.
License: Apache-2.0
""",
    },
}


def run_tool(tool: Path, *arguments: str) -> str:
    result = subprocess.run(
        [str(tool), *arguments], check=True, text=True, capture_output=True
    )
    return result.stdout.strip()


def write_json(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def file_hash(tool: Path, path: Path) -> str:
    return run_tool(tool, "hash", "--file", str(path))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_catalog_artifact(tool: Path, path: Path, artifact: dict) -> None:
    actual_bytes = path.stat().st_size
    if actual_bytes != artifact["expected_bytes"]:
        raise SystemExit(
            f"artifact size mismatch for {path}: expected "
            f"{artifact['expected_bytes']}, actual {actual_bytes}"
        )
    actual_sha256 = sha256(path)
    if actual_sha256 != artifact["sha256"]:
        raise SystemExit(
            f"artifact SHA-256 mismatch for {path}: expected "
            f"{artifact['sha256']}, actual {actual_sha256}"
        )
    actual_blake3 = file_hash(tool, path)
    if actual_blake3 != artifact["blake3"]:
        raise SystemExit(
            f"artifact BLAKE3 mismatch for {path}: expected "
            f"{artifact['blake3']}, actual {actual_blake3}"
        )


def bundle_id(model_id: str, version: str, digest: str) -> str:
    return f"{model_id}@{version}+blake3_{digest.removeprefix('blake3:')[:12]}"


def legal_text(model_id: str, catalog_model: dict, legal_root: Path,
               tool: Path) -> str:
    model_legal_root = legal_root / model_id
    license_sources = catalog_model["license_sources"]
    for source in license_sources:
        verify_catalog_artifact(tool, model_legal_root / source["path"], source)
    if len(license_sources) == 1:
        return (model_legal_root / license_sources[0]["path"]).read_text(
            encoding="utf-8"
        )
    mit = (model_legal_root / "LICENSE.pyannote").read_text(encoding="utf-8")
    apache = (model_legal_root / "LICENSE.3dspeaker").read_text(encoding="utf-8")
    return (
        "This bundle combines works under MIT and Apache-2.0.\n\n"
        "--- pyannote segmentation-3.0: MIT ---\n\n"
        + mit
        + "\n--- 3D-Speaker: Apache-2.0 ---\n\n"
        + apache
    )


def artifact_source(model_id: str, relative_path: str, source_root: Path,
                    reproduced_root: Path) -> Path:
    reproduced = reproduced_root / model_id / relative_path
    if reproduced.is_file():
        return reproduced
    return source_root / model_id / relative_path


def prepare_one(model_id: str, spec: dict, catalog_model: dict,
                source_root: Path, reproduced_root: Path, legal_root: Path,
                output_root: Path,
                tool: Path) -> dict:
    source_dir = source_root / model_id
    output_dir = output_root / model_id
    output_dir.mkdir(parents=True)

    manifest_name = spec.get("legacy_manifest", "model.svpmodel.json")
    manifest = json.loads((source_dir / manifest_name).read_text(encoding="utf-8"))

    catalog_artifacts = {
        artifact["path"]: artifact for artifact in catalog_model["artifacts"]
    }
    files = []
    for relative_path, role in spec["files"]:
        if relative_path not in catalog_artifacts:
            raise SystemExit(f"catalog is missing {model_id}/{relative_path}")
        source_path = artifact_source(
            model_id, relative_path, source_root, reproduced_root
        )
        verify_catalog_artifact(
            tool, source_path, catalog_artifacts[relative_path]
        )
        target_path = output_dir / relative_path
        target_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source_path, target_path)
        files.append({
            "path": relative_path,
            "role": role,
            "blake3": file_hash(tool, target_path),
        })

    (output_dir / "LICENSE").write_text(
        legal_text(model_id, catalog_model, legal_root, tool), encoding="utf-8"
    )
    (output_dir / "NOTICE").write_text(spec["notice"], encoding="utf-8")
    files.extend([
        {"path": "LICENSE", "role": "license",
         "blake3": file_hash(tool, output_dir / "LICENSE")},
        {"path": "NOTICE", "role": "notice",
         "blake3": file_hash(tool, output_dir / "NOTICE")},
    ])

    manifest.update({
        "model_version": catalog_model["model_version"],
        "model_bundle_id": bundle_id(
            model_id, catalog_model["model_version"], ZERO_HASH
        ),
        "bundle_blake3": ZERO_HASH,
        "source_slug": catalog_model["source_slug"],
        "source_revision": catalog_model["source_revision"],
        "license": catalog_model["license"],
        "files": files,
    })
    write_json(output_dir / "model.svpmodel.json", manifest)

    digest = run_tool(tool, "digest", "--bundle-dir", str(output_dir))
    identity = bundle_id(model_id, catalog_model["model_version"], digest)
    manifest["bundle_blake3"] = digest
    manifest["model_bundle_id"] = identity
    write_json(output_dir / "model.svpmodel.json", manifest)
    run_tool(tool, "verify", "--bundle-dir", str(output_dir))

    return {
        "model_id": model_id,
        "model_version": catalog_model["model_version"],
        "model_bundle_id": identity,
        "bundle_blake3": digest,
        "source_slug": catalog_model["source_slug"],
        "source_revision": catalog_model["source_revision"],
        "license": catalog_model["license"],
        "required_files": files,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-cache", type=Path, required=True)
    parser.add_argument("--reproduced-artifacts", type=Path, required=True)
    parser.add_argument("--legal-materials", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--models-tool", type=Path, required=True)
    parser.add_argument(
        "--catalog",
        type=Path,
        default=(Path(__file__).resolve().parents[1]
                 / "distribution/reference-models/catalog.json"),
    )
    args = parser.parse_args()

    if args.output_dir.exists():
        raise SystemExit(f"output directory already exists: {args.output_dir}")
    args.output_dir.mkdir(parents=True)

    catalog = json.loads(args.catalog.read_text(encoding="utf-8"))
    catalog_models = {model["model_id"]: model for model in catalog["models"]}
    if set(catalog_models) != set(MODELS):
        raise SystemExit("catalog must contain exactly the eight approved models")
    generated = [
        prepare_one(
            model_id, spec, catalog_models[model_id], args.source_cache,
            args.reproduced_artifacts, args.legal_materials, args.output_dir,
            args.models_tool
        )
        for model_id, spec in MODELS.items()
    ]
    root_lock = {
        "schema_version": "svp-model-lock-1",
        "model_set_id": "svp-reference-model-set-rc2",
        "models": [
            {
                "model_id": model["model_id"],
                "model_bundle_id": model["model_bundle_id"],
                "model_version": model["model_version"],
                "bundle_blake3": model["bundle_blake3"],
                "files": model["required_files"],
            }
            for model in generated
        ],
    }
    write_json(args.output_dir / "model-lock.json", root_lock)
    run_tool(
        args.models_tool, "verify", "--lock",
        str(args.output_dir / "model-lock.json"), "--cache-dir", str(args.output_dir)
    )
    write_json(args.output_dir / "release-report.json", {
        "schema_version": "svp-reference-model-release-report-1",
        "svp_version": "1.0-rc.2",
        "models": generated,
    })
    for model in generated:
        expected = catalog_models[model["model_id"]]
        expected_identity = (
            expected["model_bundle_id"], expected["bundle_blake3"]
        )
        actual_identity = (model["model_bundle_id"], model["bundle_blake3"])
        if actual_identity != expected_identity:
            raise SystemExit(
                "generated bundle identity does not match catalog for "
                f"{model['model_id']}: expected {expected_identity}, "
                f"actual {actual_identity}; see release-report.json"
            )
    print(f"prepared and verified {len(generated)} reference model bundles")


if __name__ == "__main__":
    main()
