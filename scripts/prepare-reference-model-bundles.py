#!/usr/bin/env python3
"""Assemble and verify the eight approved reference-model bundles.

This is release tooling, not an SVP runtime dependency. It never downloads or
uploads weights. The caller supplies only proven upstream-derived artifacts,
pinned legal materials, and the native verifier. Canonical manifest metadata
and NOTICE text come from committed distribution inputs. The output directory
is a complete staging tree with one authoritative root model-lock.json.
"""

import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path


ZERO_HASH = "blake3:" + ("0" * 64)
EXPECTED_MODEL_IDS = {
    "model_whisper_small_en",
    "model_sherpa_onnx_diarization",
    "model_depth_anything_v2_small",
    "model_nomic_embed_text_v1_5",
    "model_nomic_embed_vision_v1_5",
    "model_rfdetr_nano_coco",
    "model_pp_ocrv6_medium_det",
    "model_pp_ocrv6_medium_rec",
}


def run_tool(tool: Path, *arguments: str) -> str:
    result = subprocess.run(
        [str(tool), *arguments], check=True, text=True, capture_output=True
    )
    return result.stdout.strip()


def write_json(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def require_exact_keys(value: dict, expected: set[str], source: str) -> None:
    actual = set(value)
    if actual != expected:
        raise SystemExit(
            f"{source} keys mismatch: expected {sorted(expected)}, "
            f"actual {sorted(actual)}"
        )


def load_bundle_inputs(path: Path) -> dict[str, dict]:
    root = json.loads(path.read_text(encoding="utf-8"))
    require_exact_keys(root, {"schema_version", "models"}, str(path))
    if root["schema_version"] != "svp-reference-model-bundle-inputs-1":
        raise SystemExit(f"unsupported bundle-input schema in {path}")
    if not isinstance(root["models"], list):
        raise SystemExit(f"{path}.models must be an array")

    inputs = {}
    for index, model in enumerate(root["models"]):
        source = f"{path}.models[{index}]"
        require_exact_keys(model, {"model_id", "files", "notice", "manifest"}, source)
        model_id = model["model_id"]
        if not isinstance(model_id, str) or not model_id:
            raise SystemExit(f"{source}.model_id must be a non-empty string")
        if model_id in inputs:
            raise SystemExit(f"duplicate bundle input for {model_id}")
        if not isinstance(model["notice"], str) or not model["notice"].endswith("\n"):
            raise SystemExit(f"{source}.notice must be newline-terminated text")
        if not isinstance(model["manifest"], dict):
            raise SystemExit(f"{source}.manifest must be an object")
        if model["manifest"].get("model_id") != model_id:
            raise SystemExit(f"{source}.manifest.model_id must match model_id")
        if not isinstance(model["files"], list) or not model["files"]:
            raise SystemExit(f"{source}.files must be a non-empty array")
        seen_paths = set()
        for file_index, file in enumerate(model["files"]):
            file_source = f"{source}.files[{file_index}]"
            require_exact_keys(file, {"path", "role"}, file_source)
            if not all(isinstance(file[key], str) and file[key]
                       for key in ("path", "role")):
                raise SystemExit(f"{file_source} path and role must be non-empty strings")
            if file["path"] in seen_paths:
                raise SystemExit(f"duplicate bundle input path {model_id}/{file['path']}")
            seen_paths.add(file["path"])
        inputs[model_id] = model

    if set(inputs) != EXPECTED_MODEL_IDS:
        raise SystemExit("bundle inputs must contain exactly the eight approved models")
    return inputs


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
    output_dir = output_root / model_id
    output_dir.mkdir(parents=True)

    manifest = spec["manifest"].copy()

    catalog_artifacts = {
        artifact["path"]: artifact for artifact in catalog_model["artifacts"]
    }
    files = []
    for file in spec["files"]:
        relative_path = file["path"]
        role = file["role"]
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
    parser.add_argument(
        "--bundle-inputs",
        type=Path,
        default=(Path(__file__).resolve().parents[1]
                 / "distribution/reference-models/bundle-inputs.json"),
    )
    args = parser.parse_args()

    if args.output_dir.exists():
        raise SystemExit(f"output directory already exists: {args.output_dir}")
    args.output_dir.mkdir(parents=True)

    catalog = json.loads(args.catalog.read_text(encoding="utf-8"))
    catalog_models = {model["model_id"]: model for model in catalog["models"]}
    if set(catalog_models) != EXPECTED_MODEL_IDS:
        raise SystemExit("catalog must contain exactly the eight approved models")
    bundle_inputs = load_bundle_inputs(args.bundle_inputs)
    generated = [
        prepare_one(
            model_id, bundle_inputs[model_id], catalog_models[model_id],
            args.source_cache,
            args.reproduced_artifacts, args.legal_materials, args.output_dir,
            args.models_tool
        )
        for model_id in bundle_inputs
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
