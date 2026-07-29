#!/usr/bin/env python3
"""Test PP-OCRv6 ONNX models with onnxruntime on the same frames."""
import os
import time
import json
import argparse

def preprocess_det_image(img, limit_side_len=960):
    """Preprocess image for PP-OCR detector."""
    h, w = img.shape[:2]
    ratio = 1.0
    if max(h, w) > limit_side_len:
        if h > w:
            ratio = float(limit_side_len) / h
        else:
            ratio = float(limit_side_len) / w
    resized_h = int(h * ratio)
    resized_w = int(w * ratio)
    # Pad to multiple of 32 to avoid dynamic shape issues
    resized_h = int(np.ceil(resized_h / 32.0) * 32)
    resized_w = int(np.ceil(resized_w / 32.0) * 32)
    resized_img = cv2.resize(img, (resized_w, resized_h))
    resized_img = resized_img.astype(np.float32) / 255.0
    mean = np.array([0.485, 0.456, 0.406], dtype=np.float32)
    std = np.array([0.229, 0.224, 0.225], dtype=np.float32)
    resized_img = (resized_img - mean) / std
    resized_img = resized_img.transpose(2, 0, 1)
    resized_img = np.expand_dims(resized_img, axis=0)
    return resized_img, ratio

def sigmoid(x):
    return 1.0 / (1.0 + np.exp(-x))

def get_det_boxes(pred, ratio, thresh=0.3, box_thresh=0.6, unclip_ratio=1.5):
    """Simple DB post-process to get bounding boxes."""
    pred = pred[0, 0, :, :]
    segmentation = pred > thresh

    h, w = segmentation.shape
    boxes = []

    # Simple connected component approach
    num_labels, labels, stats, centroids = cv2.connectedComponentsWithStats(
        segmentation.astype(np.uint8), connectivity=8
    )

    for i in range(1, num_labels):
        x, y, bw, bh, area = stats[i]
        if area < 10:
            continue
        if pred[y:y+bh, x:x+bw].mean() < box_thresh:
            continue
        # Unclip
        x1 = max(0, int(x / ratio - bw * (unclip_ratio - 1) / 2 / ratio))
        y1 = max(0, int(y / ratio - bh * (unclip_ratio - 1) / 2 / ratio))
        x2 = int((x + bw) / ratio + bw * (unclip_ratio - 1) / 2 / ratio)
        y2 = int((y + bh) / ratio + bh * (unclip_ratio - 1) / 2 / ratio)
        boxes.append([x1, y1, x2, y2])

    return boxes

def crop_text_region(img, box, padding=0.1):
    """Crop a text region from the image given a bounding box."""
    h, w = img.shape[:2]
    x1, y1, x2, y2 = box
    pw = int((x2 - x1) * padding)
    ph = int((y2 - y1) * padding)
    x1 = max(0, x1 - pw)
    y1 = max(0, y1 - ph)
    x2 = min(w, x2 + pw)
    y2 = min(h, y2 + ph)
    return img[y1:y2, x1:x2]

def preprocess_rec_image(img, target_height=48, max_width=320):
    """Preprocess crop for PP-OCR recognizer.
    Uses BGR, image_shape [3, 48, 320], no mean/std normalization (just /255).
    """
    # Convert to BGR if needed (cv2 reads BGR by default, so no conversion needed)
    h, w = img.shape[:2]
    if h != target_height:
        ratio = float(target_height) / h
        new_w = min(int(w * ratio), max_width)
        img = cv2.resize(img, (new_w, target_height))
    # Pad to max_width if needed
    h, w = img.shape[:2]
    if w < max_width:
        pad = np.zeros((h, max_width - w, 3), dtype=np.uint8)
        img = np.hstack([img, pad])
    img = img.astype(np.float32) / 255.0
    img = img.transpose(2, 0, 1)
    img = np.expand_dims(img, axis=0)
    return img

# PP-OCRv6 character set (simplified - uses standard English + digits + common symbols)
# The actual dictionary is stored in the model's rec.yml
def load_rec_dict(yml_path=None):
    """Load recognition dictionary from inference.yml."""
    if yml_path and os.path.exists(yml_path):
        import yaml
        with open(yml_path) as f:
            data = yaml.safe_load(f)
        chars = data.get("PostProcess", {}).get("character_dict", [])
        if chars:
            return chars
    # Fallback
    return list("0123456789abcdefghijklmnopqrstuvwxyz")

def ctc_decode(pred, dict_chars):
    """CTC greedy decode."""
    pred_idx = pred.argmax(axis=2)[0]
    result = []
    prev_idx = 0  # blank
    for idx in pred_idx:
        if idx != prev_idx and idx != 0:
            if idx - 1 < len(dict_chars):
                result.append(dict_chars[idx - 1])
        prev_idx = idx
    return "".join(result)

def main():
    parser = argparse.ArgumentParser(description="PP-OCRv6 ONNX Runtime runner")
    parser.add_argument("--det-model", required=True)
    parser.add_argument("--rec-model", required=True)
    parser.add_argument("--rec-yml")
    parser.add_argument("--images", nargs="+", required=True)
    parser.add_argument("--output", default=None)
    args = parser.parse_args()

    model_paths = (
        ("--det-model", args.det_model),
        ("--rec-model", args.rec_model),
    )
    for option, model_path in model_paths:
        if not os.path.isfile(model_path):
            parser.error(f"{option} must identify an existing file")
    if args.rec_yml and not os.path.isfile(args.rec_yml):
        parser.error("--rec-yml must identify an existing file")
    for image_path in args.images:
        if not os.path.isfile(image_path):
            parser.error(f"--images entry does not exist: {image_path}")

    global np, cv2, ort
    try:
        import numpy as np
        import cv2
        import onnxruntime as ort
    except ModuleNotFoundError as error:
        parser.error(
            f"missing Python package {error.name}; install numpy, "
            "opencv-python-headless, and onnxruntime"
        )

    # Load ONNX models
    det_session = ort.InferenceSession(args.det_model, providers=['CPUExecutionProvider'])
    rec_session = ort.InferenceSession(args.rec_model, providers=['CPUExecutionProvider'])

    det_input_name = det_session.get_inputs()[0].name
    rec_input_name = rec_session.get_inputs()[0].name

    dict_chars = load_rec_dict(args.rec_yml)

    results = []
    for img_path in args.images:
        if not os.path.exists(img_path):
            continue

        img = cv2.imread(img_path)
        if img is None:
            continue

        t0 = time.time()

        # Detection
        det_input, ratio = preprocess_det_image(img)
        det_output = det_session.run(None, {det_input_name: det_input})
        det_pred = det_output[0]

        boxes = get_det_boxes(det_pred, ratio)

        # Recognition
        detections = []
        for box in boxes:
            crop = crop_text_region(img, box)
            if crop.size == 0:
                continue
            rec_input = preprocess_rec_image(crop)
            rec_output = rec_session.run(None, {rec_input_name: rec_input})
            rec_pred = rec_output[0]
            text = ctc_decode(rec_pred, dict_chars)
            if text.strip():
                detections.append({"text": text, "score": 0.0, "box": box})

        elapsed = time.time() - t0
        entry = {
            "image": img_path,
            "elapsed_seconds": round(elapsed, 3),
            "detections": detections
        }
        results.append(entry)

        print(f"\n{os.path.basename(img_path)} ({elapsed:.2f}s):")
        if not detections:
            print("  (no text detected)")
        for d in detections:
            print(f'  text="{d["text"]}" box={d["box"]}')

    output = {
        "model": "PP-OCRv6_medium_ONNX",
        "det_model": args.det_model,
        "rec_model": args.rec_model,
        "results": results
    }

    if args.output:
        with open(args.output, "w") as f:
            json.dump(output, f, indent=2)
        print(f"\nResults written to {args.output}")

if __name__ == "__main__":
    main()
