import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from model_installer.conversion import (
    convert_ppocr, create_environment, run_checked, wrap_rfdetr,
)
from model_installer.downloads import Download, run_downloads
from model_installer.progress import JsonProgress


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_file(path, expected):
    actual_bytes = path.stat().st_size
    if actual_bytes != expected["expected_bytes"]:
        raise RuntimeError(
            f"byte count mismatch for {path}: expected "
            f"{expected['expected_bytes']}, actual {actual_bytes}"
        )
    actual_hash = sha256(path)
    if actual_hash != expected["sha256"]:
        raise RuntimeError(
            f"SHA-256 mismatch for {path}: expected {expected['sha256']}, "
            f"actual {actual_hash}"
        )


def download_plan(catalog, source_root, reproduction_root, legal_root,
                  model_ids=None):
    result = []
    for model in catalog["models"]:
        model_id = model["model_id"]
        if model_ids is not None and model_id not in model_ids:
            continue
        label = model_id.removeprefix("model_")
        for source in model["license_sources"]:
            result.append(Download(
                model_id, label, source["source_url"],
                legal_root / model_id / source["path"],
                source["expected_bytes"], source["sha256"], {},
            ))
        for artifact in model["artifacts"]:
            if "source_url" not in artifact:
                continue
            result.append(Download(
                model_id, label, artifact["source_url"],
                source_root / model_id / artifact["path"],
                artifact["expected_bytes"], artifact["sha256"],
                artifact.get("request_headers", {}),
            ))
        reproduction = model.get("reproduction")
        if not reproduction:
            continue
        if "source_files" in reproduction:
            sources = reproduction["source_files"]
        else:
            sources = [reproduction["source_file"]]
        for source in sources:
            result.append(Download(
                model_id, label, source["url"],
                reproduction_root / model_id / source["path"],
                source["expected_bytes"], source["sha256"], {},
            ))
    return result


def run_tool(tool, *arguments):
    result = subprocess.run(
        [str(tool), *map(str, arguments)], text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    if result.returncode != 0:
        raise RuntimeError(result.stdout.strip())


def verify_exact_cache(tool, reference_set_path, cache_dir):
    lock_path = cache_dir / "model-lock.json"
    if not lock_path.is_file():
        raise RuntimeError(f"missing authoritative model lock: {lock_path}")
    try:
        lock = json.loads(lock_path.read_text(encoding="utf-8"))
        reference_set = json.loads(reference_set_path.read_text(encoding="utf-8"))
        locked_ids = [model["model_id"] for model in lock["models"]]
        reference_ids = [model["model_id"] for model in reference_set["models"]]
    except (KeyError, TypeError, json.JSONDecodeError) as error:
        raise RuntimeError(f"could not parse authoritative model lock: {error}") from error
    if len(locked_ids) != len(set(locked_ids)):
        raise RuntimeError("authoritative model lock contains duplicate model IDs")
    if len(reference_ids) != len(set(reference_ids)):
        raise RuntimeError("reference model set contains duplicate model IDs")
    if set(locked_ids) != set(reference_ids):
        raise RuntimeError(
            "authoritative model lock does not contain the exact reference model set"
        )
    run_tool(tool, "verify", "--lock", lock_path, "--cache-dir", cache_dir)
    run_tool(tool, "verify", "--model-set", reference_set_path,
             "--cache-dir", cache_dir)


def verify_existing_cache_subset(tool, reference_set_path, cache_dir):
    lock_path = cache_dir / "model-lock.json"
    if not lock_path.is_file():
        raise RuntimeError(f"missing authoritative model lock: {lock_path}")

    try:
        lock = json.loads(lock_path.read_text(encoding="utf-8"))
        reference_set = json.loads(reference_set_path.read_text(encoding="utf-8"))
        locked_models = lock["models"]
        reference_models = reference_set["models"]
    except (KeyError, TypeError, json.JSONDecodeError) as error:
        raise RuntimeError(f"could not parse authoritative model cache: {error}") from error

    locked_by_id = {model["model_id"]: model for model in locked_models}
    if len(locked_by_id) != len(locked_models):
        raise RuntimeError("authoritative model lock contains duplicate model IDs")
    reference_by_id = {model["model_id"]: model for model in reference_models}
    unexpected = set(locked_by_id) - set(reference_by_id)
    if unexpected:
        raise RuntimeError(
            "authoritative model lock contains models outside the current reference set: "
            + ", ".join(sorted(unexpected))
        )

    for model_id, locked in locked_by_id.items():
        reference = reference_by_id[model_id]
        if locked.get("model_bundle_id") != reference.get("model_bundle_id"):
            raise RuntimeError(
                f"existing cache bundle identity is not current for {model_id}"
            )
        if locked.get("bundle_blake3") != reference.get("bundle_blake3"):
            raise RuntimeError(
                f"existing cache bundle digest is not current for {model_id}"
            )

    run_tool(tool, "verify", "--lock", lock_path, "--cache-dir", cache_dir)
    return set(locked_by_id)


def atomic_publish(staging, destination, verify_published, replace_existing=False):
    backup = destination.with_name(destination.name + ".previous")
    if backup.exists():
        raise RuntimeError(f"stale model cache backup exists: {backup}")
    had_destination = destination.exists()
    if had_destination and not replace_existing:
        raise RuntimeError(f"model cache destination already exists: {destination}")

    if had_destination:
        destination.replace(backup)
    try:
        staging.replace(destination)
        verify_published(destination)
    except BaseException:
        if destination.exists():
            shutil.rmtree(destination)
        if had_destination:
            backup.replace(destination)
        raise
    if had_destination:
        shutil.rmtree(backup)


def run(args, progress):
    catalog = json.loads(args.catalog.read_text(encoding="utf-8"))
    if len(catalog.get("models", [])) != 9:
        raise RuntimeError("catalog must contain the exact nine-model set")
    catalog_model_ids = {model["model_id"] for model in catalog["models"]}
    total_models = len(catalog_model_ids)
    existing_model_ids = set()
    reuse_cache = args.cache_dir.exists()

    if reuse_cache:
        lock_path = args.cache_dir / "model-lock.json"
        try:
            lock = json.loads(lock_path.read_text(encoding="utf-8"))
            locked_model_ids = {
                model["model_id"] for model in lock["models"]
            }
        except (OSError, KeyError, TypeError, json.JSONDecodeError) as error:
            raise RuntimeError(
                f"could not inspect existing model cache: {error}"
            ) from error

        if locked_model_ids == catalog_model_ids:
            verify_exact_cache(args.models_tool, args.reference_set, args.cache_dir)
            progress.emit("stage_completed", "models", "Models",
                          current=total_models, total=total_models,
                          unit="models",
                          message=f"already verified at {args.cache_dir}")
            return

        existing_model_ids = verify_existing_cache_subset(
            args.models_tool, args.reference_set, args.cache_dir)

    missing_model_ids = catalog_model_ids - existing_model_ids
    staging = args.cache_dir.with_name(
        args.cache_dir.name + f".install-{os.getpid()}"
    )
    if staging.exists():
        raise RuntimeError(f"stale installer staging directory exists: {staging}")
    work_root = Path(tempfile.mkdtemp(prefix="svp-model-install-"))
    source_root = work_root / "source"
    reproduction_sources = work_root / "reproduction-source"
    reproduction_outputs = work_root / "reproduced"
    legal_root = work_root / "legal"
    try:
        needs_pp_environment = any(
            model["model_id"] in missing_model_ids and
            model.get("reproduction", {}).get("source_files")
            for model in catalog["models"]
        )
        needs_rf_environment = any(
            model["model_id"] in missing_model_ids and
            model.get("reproduction", {}).get("source_file")
            for model in catalog["models"]
        )
        pp_environment = None
        rf_environment = None
        if needs_pp_environment or needs_rf_environment:
            progress.emit("stage_started", "bootstrap", "Installer Tools")
            if needs_pp_environment:
                pp_environment = create_environment(
                    sys.executable, work_root / "ppocr-environment",
                    args.ppocr_lock)
            if needs_rf_environment:
                rf_environment = create_environment(
                    sys.executable, work_root / "rfdetr-environment",
                    args.rfdetr_lock)
            progress.emit("stage_completed", "bootstrap", "Installer Tools")

        downloads = download_plan(
            catalog, source_root, reproduction_sources, legal_root,
            missing_model_ids)
        run_downloads(downloads, args.parallel_downloads, progress)

        completed_models = len(existing_model_ids)
        for model in catalog["models"]:
            model_id = model["model_id"]
            if model_id in existing_model_ids:
                continue
            reproduction = model.get("reproduction")
            if reproduction and "source_files" in reproduction:
                artifact = next(
                    value for value in model["artifacts"]
                    if value["path"].endswith(".onnx")
                )
                output = reproduction_outputs / model_id / artifact["path"]
                output.parent.mkdir(parents=True, exist_ok=True)
                progress.emit("stage_started", "reproduce", "Reproduce",
                              scope_id=model_id,
                              scope_label=model_id.removeprefix("model_"))
                convert_ppocr(
                    pp_environment,
                    reproduction_sources / model_id,
                    output,
                )
                verify_file(output, artifact)
                progress.emit("stage_completed", "reproduce", "Reproduce",
                              scope_id=model_id,
                              scope_label=model_id.removeprefix("model_"))
            elif reproduction and "source_file" in reproduction:
                artifact = model["artifacts"][0]
                output = reproduction_outputs / model_id / artifact["path"]
                output.parent.mkdir(parents=True, exist_ok=True)
                progress.emit("stage_started", "reproduce", "Reproduce",
                              scope_id=model_id,
                              scope_label=model_id.removeprefix("model_"))
                wrap_rfdetr(
                    rf_environment,
                    reproduction_sources / model_id /
                    reproduction["source_file"]["path"],
                    output, reproduction,
                )
                verify_file(output, artifact)
                progress.emit("stage_completed", "reproduce", "Reproduce",
                              scope_id=model_id,
                              scope_label=model_id.removeprefix("model_"))
            completed_models += 1
            progress.emit("stage_progress", "models", "Models",
                          current=completed_models, total=total_models, unit="models",
                          scope_id="aggregate-models")

        progress.emit("stage_completed", "models", "Models",
                      current=completed_models, total=total_models, unit="models",
                      scope_id="aggregate-models")

        args.cache_dir.parent.mkdir(parents=True, exist_ok=True)
        progress.emit("stage_started", "publish", "Model Cache")
        prepare_command = [
            sys.executable, args.prepare_script,
            "--source-cache", source_root,
            "--reproduced-artifacts", reproduction_outputs,
            "--legal-materials", legal_root,
            "--output-dir", staging,
            "--models-tool", args.models_tool,
            "--catalog", args.catalog,
            "--bundle-inputs", args.bundle_inputs,
        ]
        if reuse_cache:
            prepare_command.extend(["--reuse-cache", args.cache_dir])
        run_checked(prepare_command)
        verify_exact_cache(args.models_tool, args.reference_set, staging)
        atomic_publish(
            staging, args.cache_dir,
            lambda published: verify_exact_cache(
                args.models_tool, args.reference_set, published),
            replace_existing=reuse_cache,
        )
        progress.emit("stage_completed", "publish", "Model Cache",
                      message="\n" + str(args.cache_dir))
    finally:
        shutil.rmtree(work_root, ignore_errors=True)
        if staging.exists():
            shutil.rmtree(staging, ignore_errors=True)


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--bundle-inputs", type=Path, required=True)
    parser.add_argument("--reference-set", type=Path, required=True)
    parser.add_argument("--prepare-script", type=Path, required=True)
    parser.add_argument("--ppocr-lock", type=Path, required=True)
    parser.add_argument("--rfdetr-lock", type=Path, required=True)
    parser.add_argument("--models-tool", type=Path, required=True)
    parser.add_argument("--cache-dir", type=Path, required=True)
    parser.add_argument("--parallel-downloads", type=int, choices=(1, 2), default=1)
    return parser.parse_args()


def main():
    args = parse_args()
    progress = JsonProgress(sys.stdout)
    try:
        run(args, progress)
    except KeyboardInterrupt:
        progress.emit("stage_failed", "models", "Models",
                      message="cancelled")
        return 130
    except BaseException as error:
        progress.emit("stage_failed", "models", "Models", message=str(error))
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
