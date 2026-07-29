import math
import os
import subprocess
from pathlib import Path


def run_checked(arguments, *, environment=None):
    result = subprocess.run(
        [str(item) for item in arguments], text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=environment,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {arguments[0]}\n{result.stdout}"
        )


def create_environment(python, destination, requirements):
    run_checked([python, "-m", "venv", destination])
    environment_python = destination / "bin" / "python3"
    run_checked([
        environment_python, "-m", "pip", "install", "--disable-pip-version-check",
        "--no-cache-dir", "--only-binary=:all:", "--no-deps",
        "--require-hashes", "-r", requirements,
    ])
    return environment_python


def convert_ppocr(environment_python, source_dir, output_path):
    converter = environment_python.parent / "paddle2onnx"
    run_checked([
        converter, "--model_dir", source_dir,
        "--model_filename", "inference.json",
        "--params_filename", "inference.pdiparams",
        "--save_file", output_path, "--opset_version", "14",
    ])


def _resolved_shape(output, expected):
    actual = []
    for index, (dimension, expected_value) in enumerate(
            zip(output.type.tensor_type.shape.dim, expected)):
        if dimension.HasField("dim_value") and dimension.dim_value > 0:
            value = dimension.dim_value
        elif index == 0 and dimension.dim_param and expected_value == 1:
            value = 1
        else:
            raise RuntimeError("RF-DETR output has an unsupported dynamic shape")
        if value != expected_value:
            raise RuntimeError("RF-DETR output shape changed")
        actual.append(value)
    return actual


def wrap_rfdetr(environment_python, source_path, output_path, reproduction):
    helper_script = output_path.parent / ".wrap-rfdetr.py"
    helper_script.write_text(
        "from model_installer.conversion import _wrap_rfdetr_main\n"
        "_wrap_rfdetr_main()\n", encoding="utf-8"
    )
    environment = os.environ.copy()
    module_root = str(Path(__file__).resolve().parents[1])
    environment["PYTHONPATH"] = module_root
    environment["SVP_RF_SOURCE"] = str(source_path)
    environment["SVP_RF_OUTPUT"] = str(output_path)
    environment["SVP_RF_REPRODUCTION"] = __import__("json").dumps(reproduction)
    try:
        run_checked([environment_python, helper_script], environment=environment)
    finally:
        helper_script.unlink(missing_ok=True)


def _wrap_rfdetr_main():
    import json
    import onnx
    from onnx import TensorProto, helper

    source_path = Path(os.environ["SVP_RF_SOURCE"])
    output_path = Path(os.environ["SVP_RF_OUTPUT"])
    reproduction = json.loads(os.environ["SVP_RF_REPRODUCTION"])
    wrapped = onnx.load(source_path)
    if [output.name for output in wrapped.graph.output] != reproduction["output_order"]:
        raise RuntimeError("RF-DETR upstream output order changed")
    contract = reproduction["output_contract"]
    if contract["operation"] != "flatten_and_concatenate":
        raise RuntimeError("unsupported RF-DETR wrapper operation")
    components = []
    for output, expected in zip(wrapped.graph.output, contract["components"]):
        if output.type.tensor_type.elem_type != TensorProto.FLOAT:
            raise RuntimeError("RF-DETR output is not FP32")
        components.append({"name": output.name,
                           "shape": _resolved_shape(output, expected["shape"])})
    if components != contract["components"]:
        raise RuntimeError("RF-DETR output contract changed")
    count = sum(math.prod(component["shape"]) for component in components)
    if count != contract["combined_element_count"]:
        raise RuntimeError("RF-DETR combined output size changed")
    flattened = []
    for output in list(wrapped.graph.output):
        shape_name = output.name + "_svp_flat_shape"
        flat_name = output.name + "_svp_flat"
        wrapped.graph.initializer.append(
            helper.make_tensor(shape_name, TensorProto.INT64, [1], [-1]))
        wrapped.graph.node.append(helper.make_node(
            "Reshape", [output.name, shape_name], [flat_name],
            name=output.name + "_svp_flatten"))
        flattened.append(flat_name)
    wrapped.graph.node.append(helper.make_node(
        "Concat", flattened, ["detections"], axis=0,
        name="svp_combine_detection_outputs"))
    del wrapped.graph.output[:]
    wrapped.graph.output.append(helper.make_tensor_value_info(
        contract["combined_name"], TensorProto.FLOAT, [count]))
    wrapped.doc_string = (
        "SVP wrapper around onnx-community/rfdetr_nano-ONNX FP32. "
        "The learned weights and input are unchanged; pred_boxes and logits are "
        "flattened and concatenated into one output tensor."
    )
    onnx.checker.check_model(wrapped)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    onnx.save(wrapped, output_path)
