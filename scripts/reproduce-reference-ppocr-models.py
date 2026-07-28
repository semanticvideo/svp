#!/usr/bin/env python3
"""Reproduce the exact production PP-OCR ONNX artifacts from pinned sources."""

import argparse
import hashlib
import importlib.metadata
import json
import subprocess
import sys
import urllib.request
from pathlib import Path


PACKAGE_VERSIONS = {
    "paddle2onnx": "2.1.0",
    "paddlepaddle": "3.0.0",
    "onnx": "1.17.0",
    "onnxruntime": "1.19.2",
    "polygraphy": "0.50.3",
    "onnx-graphsurgeon": "0.6.1",
    "numpy": "2.4.6",
    "protobuf": "7.35.1",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require_environment() -> None:
    if sys.version_info[:2] != (3, 11):
        raise SystemExit("PP-OCR reproduction requires Python 3.11")
    for package, expected in PACKAGE_VERSIONS.items():
        actual = importlib.metadata.version(package)
        if actual != expected:
            raise SystemExit(
                f"PP-OCR reproduction requires {package}=={expected}; found {actual}"
            )


def require_size(path: Path, expected_bytes: int) -> None:
    actual = path.stat().st_size
    if actual != expected_bytes:
        raise SystemExit(
            f"byte-size mismatch for {path}: expected {expected_bytes}, actual {actual}"
        )


def download(url: str, destination: Path, expected_sha256: str,
             expected_bytes: int) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(url) as response, destination.open("wb") as output:
        while chunk := response.read(1024 * 1024):
            output.write(chunk)
    require_size(destination, expected_bytes)
    actual = sha256(destination)
    if actual != expected_sha256:
        raise SystemExit(
            f"source SHA-256 mismatch for {destination}: "
            f"expected {expected_sha256}, actual {actual}"
        )


def blake3(models_tool: Path, path: Path) -> str:
    return subprocess.run(
        [str(models_tool), "hash", "--file", str(path)],
        check=True,
        text=True,
        capture_output=True,
    ).stdout.strip()


def reproduce(model: dict, source_root: Path, output_root: Path,
              models_tool: Path) -> None:
    model_id = model["model_id"]
    reproduction = model["reproduction"]
    source_dir = source_root / model_id
    for source_file in reproduction["source_files"]:
        download(
            source_file["url"],
            source_dir / source_file["path"],
            source_file["sha256"],
            source_file["expected_bytes"],
        )
        actual_source_blake3 = blake3(
            models_tool, source_dir / source_file["path"]
        )
        if actual_source_blake3 != source_file["blake3"]:
            raise SystemExit(
                f"source BLAKE3 mismatch for {model_id}/{source_file['path']}"
            )

    generated = next(
        artifact for artifact in model["artifacts"]
        if artifact["path"].endswith(".onnx")
    )
    output_path = output_root / model_id / generated["path"]
    output_path.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            str(Path(sys.executable).parent / "paddle2onnx"),
            "--model_dir", str(source_dir),
            "--model_filename", "inference.json",
            "--params_filename", "inference.pdiparams",
            "--save_file", str(output_path),
            "--opset_version", "14",
        ],
        check=True,
    )
    actual_sha256 = sha256(output_path)
    require_size(output_path, generated["expected_bytes"])
    if actual_sha256 != generated["sha256"]:
        raise SystemExit(
            f"output SHA-256 mismatch for {model_id}: expected "
            f"{generated['sha256']}, actual {actual_sha256}"
        )
    actual_blake3 = blake3(models_tool, output_path)
    if actual_blake3 != generated["blake3"]:
        raise SystemExit(
            f"output BLAKE3 mismatch for {model_id}: expected "
            f"{generated['blake3']}, actual {actual_blake3}"
        )

    for artifact in model["artifacts"]:
        if artifact["path"].endswith(".onnx"):
            continue
        if not artifact.get("source_url"):
            raise SystemExit(f"missing source URL for {model_id}/{artifact['path']}")
        download(
            artifact["source_url"],
            output_root / model_id / artifact["path"],
            artifact["sha256"],
            artifact["expected_bytes"],
        )
        if blake3(models_tool, output_root / model_id / artifact["path"]) != artifact["blake3"]:
            raise SystemExit(f"output BLAKE3 mismatch for {model_id}/{artifact['path']}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--models-tool", type=Path, required=True)
    args = parser.parse_args()

    if args.work_dir.exists() or args.output_dir.exists():
        raise SystemExit("work and output directories must not already exist")
    args.work_dir.mkdir(parents=True)
    args.output_dir.mkdir(parents=True)
    require_environment()

    catalog = json.loads(args.catalog.read_text(encoding="utf-8"))
    models = [
        model for model in catalog["models"]
        if model["model_id"].startswith("model_pp_ocrv6_")
    ]
    if len(models) != 2:
        raise SystemExit("distribution catalog must contain exactly two PP-OCR models")
    for model in models:
        reproduce(model, args.work_dir, args.output_dir, args.models_tool)
    print("reproduced and verified the exact two production PP-OCR ONNX artifacts")


if __name__ == "__main__":
    main()
