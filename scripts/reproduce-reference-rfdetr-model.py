#!/usr/bin/env python3
"""Reproduce the approved RF-DETR output wrapper from its pinned FP32 graph."""

import argparse
import hashlib
import importlib.metadata
import json
import math
import platform
import subprocess
import urllib.request
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


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
            f"source SHA-256 mismatch: expected {expected_sha256}, actual {actual}"
        )


def blake3(models_tool: Path, path: Path) -> str:
    return subprocess.run(
        [str(models_tool), "hash", "--file", str(path)],
        check=True,
        text=True,
        capture_output=True,
    ).stdout.strip()


def require_toolchain(expected: dict) -> None:
    actual_python = platform.python_version()
    if actual_python != expected["python"]:
        raise SystemExit(
            f"RF-DETR reproduction requires Python {expected['python']}; "
            f"found {actual_python}"
        )
    for package in ("onnx", "protobuf", "numpy", "ml_dtypes", "typing_extensions"):
        actual = importlib.metadata.version(package)
        if actual != expected[package]:
            raise SystemExit(
                f"RF-DETR reproduction requires {package}=={expected[package]}; "
                f"found {actual}"
            )


def resolved_tensor_shape(output, expected_shape: list[int]) -> list[int]:
    declared = output.type.tensor_type.shape.dim
    if len(declared) != len(expected_shape):
        raise SystemExit(
            f"RF-DETR output {output.name} rank changed: expected "
            f"{len(expected_shape)}, actual {len(declared)}"
        )
    dimensions = []
    for index, (dimension, expected) in enumerate(zip(declared, expected_shape)):
        if dimension.HasField("dim_value") and dimension.dim_value > 0:
            actual = dimension.dim_value
        elif index == 0 and dimension.dim_param and expected == 1:
            actual = expected
        else:
            raise SystemExit(
                f"RF-DETR output {output.name} dimension {index} is not a "
                "supported fixed or single-batch dimension"
            )
        if actual != expected:
            raise SystemExit(
                f"RF-DETR output {output.name} dimension {index} changed: "
                f"expected {expected}, actual {actual}"
            )
        dimensions.append(actual)
    return dimensions


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

    catalog = json.loads(args.catalog.read_text(encoding="utf-8"))
    model = next(
        item for item in catalog["models"]
        if item["model_id"] == "model_rfdetr_nano_coco"
    )
    reproduction = model["reproduction"]
    require_toolchain(reproduction["toolchain"])
    source_file = reproduction["source_file"]
    source_path = args.work_dir / "model.onnx"
    download(
        source_file["url"], source_path, source_file["sha256"],
        source_file["expected_bytes"]
    )
    if blake3(args.models_tool, source_path) != source_file["blake3"]:
        raise SystemExit("RF-DETR source BLAKE3 mismatch")

    import onnx
    from onnx import TensorProto, helper

    wrapped = onnx.load(source_path)
    output_names = [output.name for output in wrapped.graph.output]
    if output_names != reproduction["output_order"]:
        raise SystemExit(f"unexpected upstream output order: {output_names}")

    output_contract = reproduction["output_contract"]
    if output_contract["operation"] != "flatten_and_concatenate":
        raise SystemExit("unsupported RF-DETR wrapper output operation")
    expected_components = output_contract["components"]
    actual_components = []
    for output, expected_component in zip(wrapped.graph.output, expected_components):
        if output.type.tensor_type.elem_type != TensorProto.FLOAT:
            raise SystemExit(f"RF-DETR output {output.name} is not FP32")
        actual_components.append({
            "name": output.name,
            "shape": resolved_tensor_shape(output, expected_component["shape"]),
        })
    if actual_components != expected_components:
        raise SystemExit(
            "RF-DETR upstream output contract changed: "
            f"expected {expected_components}, actual {actual_components}"
        )
    combined_element_count = sum(
        math.prod(component["shape"]) for component in actual_components
    )
    if combined_element_count != output_contract["combined_element_count"]:
        raise SystemExit(
            "RF-DETR combined output element count changed: "
            f"expected {output_contract['combined_element_count']}, "
            f"actual {combined_element_count}"
        )

    flattened = []
    for output in list(wrapped.graph.output):
        shape_name = output.name + "_svp_flat_shape"
        flat_name = output.name + "_svp_flat"
        wrapped.graph.initializer.append(helper.make_tensor(
            shape_name, TensorProto.INT64, [1], [-1]))
        wrapped.graph.node.append(helper.make_node(
            "Reshape", [output.name, shape_name], [flat_name],
            name=output.name + "_svp_flatten"))
        flattened.append(flat_name)

    wrapped.graph.node.append(helper.make_node(
        "Concat", flattened, ["detections"], axis=0,
        name="svp_combine_detection_outputs"))
    del wrapped.graph.output[:]
    wrapped.graph.output.append(helper.make_tensor_value_info(
        output_contract["combined_name"], TensorProto.FLOAT,
        [combined_element_count]))
    wrapped.doc_string = (
        "SVP wrapper around onnx-community/rfdetr_nano-ONNX FP32. "
        "The learned weights and input are unchanged; pred_boxes and logits are "
        "flattened and concatenated into one output tensor."
    )
    onnx.checker.check_model(wrapped)

    artifact = model["artifacts"][0]
    output_path = args.output_dir / model["model_id"] / artifact["path"]
    output_path.parent.mkdir(parents=True)
    onnx.save(wrapped, output_path)
    require_size(output_path, artifact["expected_bytes"])
    actual_sha256 = sha256(output_path)
    actual_blake3 = blake3(args.models_tool, output_path)
    if actual_sha256 != artifact["sha256"] or actual_blake3 != artifact["blake3"]:
        raise SystemExit(
            "RF-DETR output identity mismatch: "
            f"SHA-256 {actual_sha256}, BLAKE3 {actual_blake3}"
        )
    print("reproduced and verified the approved RF-DETR FP32 wrapper")


if __name__ == "__main__":
    main()
