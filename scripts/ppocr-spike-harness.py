#!/usr/bin/env python3
"""
SVP PP-OCR Spike Harness

Runs both PP-OCRv6 (PaddlePaddle) and Tesseract on deterministic frames
extracted from SVP-TEST.MOV and test-30.mp4, then scores against ground truth.

Ground truth is used ONLY for scoring, never for OCR or crop generation.

Usage:
    python3 scripts/ppocr-spike-harness.py \
        --samples-dir /path/to/samples \
        --output-dir /tmp/ppocr-spike/results

Requirements:
    pip3 install paddleocr paddle2onnx onnxruntime opencv-python-headless pyyaml

Models:
    PP-OCRv6_medium_det and PP-OCRv6_medium_rec from PaddleX (Apache-2.0)
    Cached at ~/.paddlex/official_models/
"""
import subprocess
import json
import os
import sys
import time
import argparse
import hashlib
from pathlib import Path

# Ground truth for scoring only
GROUND_TRUTH = {
    "SVP-TEST.MOV": ["SVP TEST", "Hi Codex!"],
    "test-30.mp4": ["SVP TEST", "$19.99", "Phoenix, AZ", "June 20, 2026"],
}

def shasum_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(8192), b""):
            h.update(chunk)
    return h.hexdigest()

def extract_frames(video_path, timestamps, out_dir, scale=None):
    """Extract frames at given timestamps using ffmpeg."""
    os.makedirs(out_dir, exist_ok=True)
    frames = []
    for ts in timestamps:
        name = "frame_{:.1f}s.jpg".format(ts)
        out_path = os.path.join(out_dir, name)
        vf = "scale={}".format(scale) if scale else "null"
        cmd = [
            "ffmpeg", "-v", "error",
            "-ss", str(ts),
            "-i", video_path,
            "-vf", vf,
            "-vframes", "1",
            "-y", out_path
        ]
        subprocess.run(cmd, capture_output=True, timeout=30)
        if os.path.exists(out_path):
            frames.append({"path": out_path, "timestamp": ts})
    return frames

def run_ppocr(images, det_model, rec_model):
    """Run PP-OCRv6 on a list of images."""
    from paddleocr import PaddleOCR

    ocr = PaddleOCR(
        use_doc_orientation_classify=False,
        use_doc_unwarping=False,
        use_textline_orientation=False,
        text_detection_model_dir=det_model,
        text_recognition_model_dir=rec_model,
    )

    results = []
    for img in images:
        path = img["path"] if isinstance(img, dict) else img
        t0 = time.time()
        result = ocr.predict(path)
        elapsed = time.time() - t0

        texts = []
        scores = []
        polys = []
        for r in result:
            if isinstance(r, dict):
                res = r.get("res", r)
            else:
                res = getattr(r, "json", {}).get("res", {})
            texts = res.get("rec_texts", [])
            scores = res.get("rec_scores", [])
            polys = res.get("dt_polys", [])

        detections = []
        for i, (t, s) in enumerate(zip(texts, scores)):
            detections.append({
                "text": t,
                "score": round(float(s), 4),
                "poly": polys[i] if i < len(polys) and isinstance(polys[i], list) else []
            })

        results.append({
            "image": path,
            "source": img.get("source", "unknown"),
            "timestamp": img.get("timestamp", 0),
            "elapsed_seconds": round(elapsed, 3),
            "detections": detections
        })
    return results

def run_tesseract(images, tesseract_path="tesseract", language="eng"):
    """Run Tesseract on a list of images with PSM fallback."""
    psm_modes = [11, 6, 3, 7]
    results = []

    for img in images:
        path = img["path"] if isinstance(img, dict) else img
        t0 = time.time()
        all_words = []

        for psm in psm_modes:
            cmd = [tesseract_path, path, "stdout", "--psm", str(psm), "-l", language, "tsv"]
            try:
                proc = subprocess.run(cmd, capture_output=True, timeout=30)
                stdout = proc.stdout.decode("utf-8", errors="replace")
                if proc.returncode != 0:
                    continue

                lines = stdout.strip().split("\n")
                if not lines:
                    continue

                header = lines[0].split("\t")
                try:
                    text_idx = header.index("text")
                    conf_idx = header.index("conf")
                    level_idx = header.index("level")
                except ValueError:
                    continue

                for line in lines[1:]:
                    cols = line.split("\t")
                    if len(cols) <= max(text_idx, conf_idx, level_idx):
                        continue
                    try:
                        if int(cols[level_idx]) != 5:
                            continue
                        text = cols[text_idx].strip()
                        if not text:
                            continue
                        conf = int(cols[conf_idx])
                        if conf < 0:
                            continue
                        is_dup = any(w["text"] == text for w in all_words)
                        if not is_dup:
                            all_words.append({"text": text, "score": conf / 100.0, "psm": psm})
                    except (ValueError, IndexError):
                        continue

                if all_words:
                    break
            except subprocess.TimeoutExpired:
                continue

        elapsed = time.time() - t0
        results.append({
            "image": path,
            "source": img.get("source", "unknown"),
            "timestamp": img.get("timestamp", 0),
            "elapsed_seconds": round(elapsed, 3),
            "detections": all_words
        })
    return results

def normalize_for_compare(s):
    """Normalize text for comparison: lowercase, strip, collapse spaces."""
    return " ".join(s.lower().strip().split())

def score_against_truth(results, ground_truth_strings):
    """Score OCR results against ground truth strings."""
    all_text = []
    for r in results:
        for d in r["detections"]:
            all_text.append(d["text"])

    scored = []
    for gt in ground_truth_strings:
        gt_norm = normalize_for_compare(gt)
        best_match = None
        best_score = "failed"
        best_conf = 0.0

        for t in all_text:
            t_norm = normalize_for_compare(t)
            if t_norm == gt_norm:
                best_match = t
                best_score = "exact"
                break
            elif gt_norm in t_norm or t_norm in gt_norm:
                if best_score != "exact":
                    best_match = t
                    best_score = "near"
            elif len(gt_norm) > 3 and gt_norm.replace(" ", "") == t_norm.replace(" ", ""):
                if best_score != "exact":
                    best_match = t
                    best_score = "near"
            elif len(gt_norm) > 3:
                gt_words = set(gt_norm.split())
                t_words = set(t_norm.split())
                overlap = gt_words & t_words
                if len(overlap) >= len(gt_words) * 0.5 and best_score == "failed":
                    best_match = t
                    best_score = "partial"

        if best_match:
            for r in results:
                for d in r["detections"]:
                    if d["text"] == best_match:
                        best_conf = d.get("score", d.get("confidence", 0.0))
                        break

        scored.append({
            "ground_truth": gt,
            "best_match": best_match,
            "score": best_score,
            "confidence": round(best_conf, 4)
        })

    false_positives = []
    for r in results:
        for d in r["detections"]:
            t_norm = normalize_for_compare(d["text"])
            matched = False
            for gt in ground_truth_strings:
                gt_norm = normalize_for_compare(gt)
                if t_norm == gt_norm or gt_norm in t_norm or t_norm in gt_norm or gt_norm.replace(" ", "") == t_norm.replace(" ", ""):
                    matched = True
                    break
            if not matched and len(t_norm) > 1:
                false_positives.append({
                    "text": d["text"],
                    "score": d.get("score", d.get("confidence", 0.0)),
                    "image": os.path.basename(r["image"])
                })

    return {
        "fields": scored,
        "false_positives": false_positives,
        "exact_count": sum(1 for s in scored if s["score"] == "exact"),
        "near_count": sum(1 for s in scored if s["score"] == "near"),
        "partial_count": sum(1 for s in scored if s["score"] == "partial"),
        "failed_count": sum(1 for s in scored if s["score"] == "failed"),
        "false_positive_count": len(false_positives)
    }

def main():
    parser = argparse.ArgumentParser(description="SVP PP-OCR Spike Harness")
    parser.add_argument("--samples-dir", default="/Users/domesposito/Projects/samples")
    parser.add_argument("--output-dir", default="/tmp/ppocr-spike/results")
    parser.add_argument("--det-model", default="/Users/domesposito/.paddlex/official_models/PP-OCRv6_medium_det")
    parser.add_argument("--rec-model", default="/Users/domesposito/.paddlex/official_models/PP-OCRv6_medium_rec")
    parser.add_argument("--tesseract", default="tesseract")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--skip-ppocr", action="store_true")
    parser.add_argument("--skip-tesseract", action="store_true")
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)

    videos = {
        "SVP-TEST.MOV": {
            "path": os.path.join(args.samples_dir, "SVP-TEST.MOV"),
            "timestamps": [1.0, 2.0, 3.0, 4.0],
            "scale": "960:-1"
        },
        "test-30.mp4": {
            "path": os.path.join(args.samples_dir, "test-30.mp4"),
            "timestamps": [3.0, 5.0, 10.0, 15.0, 20.0, 25.0],
            "scale": "1280:-1"
        }
    }

    videos_fullres = {
        "SVP-TEST.MOV": {
            "path": os.path.join(args.samples_dir, "SVP-TEST.MOV"),
            "timestamps": [1.0, 2.0, 3.0, 4.0],
            "scale": None
        },
        "test-30.mp4": {
            "path": os.path.join(args.samples_dir, "test-30.mp4"),
            "timestamps": [3.0, 5.0, 10.0, 15.0, 20.0, 25.0],
            "scale": None
        }
    }

    all_frames = []
    for video_name, config in videos.items():
        if not os.path.exists(config["path"]):
            p = config["path"]
            print("WARNING: {} not found, skipping".format(p))
            continue
        frame_dir = os.path.join(args.output_dir, "frames", video_name.replace(".", "_"))
        frames = extract_frames(config["path"], config["timestamps"], frame_dir, config["scale"])
        for f in frames:
            f["source"] = video_name
        all_frames.extend(frames)
        print("Extracted {} frames from {}".format(len(frames), video_name))

    if not all_frames:
        print("ERROR: No frames extracted")
        sys.exit(1)

    all_frames_fullres = []
    for video_name, config in videos_fullres.items():
        if not os.path.exists(config["path"]):
            continue
        frame_dir = os.path.join(args.output_dir, "frames_fullres", video_name.replace(".", "_"))
        frames = extract_frames(config["path"], config["timestamps"], frame_dir, config["scale"])
        for f in frames:
            f["source"] = video_name
        all_frames_fullres.extend(frames)
        print("Extracted {} full-res frames from {}".format(len(frames), video_name))

    report = {
        "spike_date": "2026-06-21",
        "platform": "M2 MacBook Air",
        "models": {
            "ppocr": {
                "name": "PP-OCRv6_medium",
                "det_model": args.det_model,
                "rec_model": args.rec_model,
                "det_model_sha256": shasum_file(os.path.join(args.det_model, "inference.pdiparams")),
                "rec_model_sha256": shasum_file(os.path.join(args.rec_model, "inference.pdiparams")),
                "license": "Apache-2.0"
            },
            "tesseract": {
                "version": subprocess.run([args.tesseract, "--version"], capture_output=True, text=True).stderr.split("\n")[0],
            }
        },
        "frames": [{"path": f["path"], "source": f["source"], "timestamp": f["timestamp"]} for f in all_frames],
        "results": {},
        "scoring": {}
    }

    if not args.skip_ppocr:
        print("\n=== Running PP-OCRv6 ===")
        ppocr_results = run_ppocr(all_frames, args.det_model, args.rec_model)
        report["results"]["ppocr"] = ppocr_results

        for r in ppocr_results:
            print("\n{} ({:.2f}s):".format(os.path.basename(r["image"]), r["elapsed_seconds"]))
            if not r["detections"]:
                print("  (no text detected)")
            for d in r["detections"]:
                t = d["text"]
                s = d["score"]
                print('  text="{}" score={}'.format(t, s))

    if not args.skip_tesseract:
        print("\n=== Running Tesseract (downscaled frames) ===")
        tess_results = run_tesseract(all_frames, args.tesseract)
        report["results"]["tesseract_downscaled"] = tess_results

        for r in tess_results:
            print("\n{} ({:.2f}s):".format(os.path.basename(r["image"]), r["elapsed_seconds"]))
            if not r["detections"]:
                print("  (no text detected)")
            for d in r["detections"]:
                t = d["text"]
                s = d["score"]
                psm = d["psm"]
                print('  text="{}" conf={:.4f} psm={}'.format(t, s, psm))

    if not args.skip_tesseract and all_frames_fullres:
        print("\n=== Running Tesseract (full-resolution frames) ===")
        tess_full_results = run_tesseract(all_frames_fullres, args.tesseract)
        report["results"]["tesseract_fullres"] = tess_full_results

        for r in tess_full_results:
            print("\n{} ({:.2f}s):".format(os.path.basename(r["image"]), r["elapsed_seconds"]))
            if not r["detections"]:
                print("  (no text detected)")
            for d in r["detections"]:
                t = d["text"]
                s = d["score"]
                psm = d["psm"]
                print('  text="{}" conf={:.4f} psm={}'.format(t, s, psm))

    print("\n=== Scoring ===")
    for video_name, gt_strings in GROUND_TRUTH.items():
        print("\n{}:".format(video_name))
        for model_name in ["ppocr", "tesseract_downscaled", "tesseract_fullres"]:
            if model_name not in report["results"]:
                continue
            model_results = [r for r in report["results"][model_name] if r["source"] == video_name]
            scoring = score_against_truth(model_results, gt_strings)
            report["scoring"]["{}_{}".format(video_name, model_name)] = scoring
            print("  {}: exact={} near={} partial={} failed={} fp={}".format(
                model_name, scoring["exact_count"], scoring["near_count"],
                scoring["partial_count"], scoring["failed_count"],
                scoring["false_positive_count"]))
            for field in scoring["fields"]:
                gt = field["ground_truth"]
                sc = field["score"]
                m = field["best_match"]
                c = field["confidence"]
                print('    GT="{}" -> {} (match="{}", conf={})'.format(gt, sc, m, c))
            if scoring["false_positives"]:
                print("    False positives:")
                for fp in scoring["false_positives"]:
                    fp_t = fp["text"]
                    fp_s = fp["score"]
                    fp_i = fp["image"]
                    print('      "{}" score={} ({})'.format(fp_t, fp_s, fp_i))

    report_path = os.path.join(args.output_dir, "spike_report.json")
    with open(report_path, "w") as f:
        json.dump(report, f, indent=2)
    print("\nFull report written to {}".format(report_path))

if __name__ == "__main__":
    main()
