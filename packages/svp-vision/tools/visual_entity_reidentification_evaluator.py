#!/usr/bin/env python3
"""Evaluate visual identity embeddings from track-aware benchmark output."""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import json
import math
from pathlib import Path


DEFAULT_THRESHOLDS = (0.80, 0.85, 0.90, 0.925, 0.95, 0.97, 0.98, 0.99)
DEFAULT_MARGINS = (0.00, 0.01, 0.02, 0.03, 0.05)


def cosine_similarity(left: list[float], right: list[float]) -> float:
    if not left or len(left) != len(right):
        return -1.0
    dot = sum(a * b for a, b in zip(left, right))
    left_norm = math.sqrt(sum(value * value for value in left))
    right_norm = math.sqrt(sum(value * value for value in right))
    if left_norm <= 0.0 or right_norm <= 0.0:
        return -1.0
    return dot / (left_norm * right_norm)


def directional_consensus(
    previous: list[list[float]],
    current: list[list[float]],
    minimum_observations: int,
) -> float:
    if len(previous) < minimum_observations or not current:
        return -1.0
    best = -1.0
    for current_embedding in current:
        similarities = sorted(
            (
                cosine_similarity(previous_embedding, current_embedding)
                for previous_embedding in previous
            ),
            reverse=True,
        )
        if len(similarities) >= minimum_observations:
            best = max(best, similarities[minimum_observations - 1])
    return best


def bidirectional_consensus(
    left: list[list[float]],
    right: list[list[float]],
    minimum_observations: int,
) -> float:
    return min(
        directional_consensus(left, right, minimum_observations),
        directional_consensus(right, left, minimum_observations),
    )


def _track_appearances(
    diagnostic: dict[str, object],
    benchmark: dict[str, object],
    minimum_observations: int,
) -> list[dict[str, object]]:
    diagnostic_timestamp_offset_us = int(
        benchmark.get("policy", {}).get("diagnostic_timestamp_offset_us", 0)
    )
    timestamp_tolerance_us = int(
        benchmark.get("policy", {}).get("timestamp_tolerance_us", 0)
    )
    regions_by_track: dict[str, list[dict[str, object]]] = defaultdict(list)
    for region in diagnostic.get("regions", []):
        if region.get("embedding"):
            regions_by_track[str(region["track_id"])].append(region)

    appearances: list[dict[str, object]] = []
    for reference in benchmark.get("references", []):
        for sequence_index, track in enumerate(reference.get("tracks", [])):
            output_track_id = str(track.get("dominant_output_track_id", ""))
            regions = regions_by_track.get(output_track_id, [])
            if "start_us" in track and "end_us" in track:
                start_us = int(track["start_us"])
                end_us = int(track["end_us"])
                regions = [
                    region for region in regions
                    if start_us - timestamp_tolerance_us <= (
                        int(region["pts_us"]) + diagnostic_timestamp_offset_us
                    ) <= end_us + timestamp_tolerance_us
                ]
            embeddings = [region["embedding"] for region in regions]
            if len(embeddings) < minimum_observations:
                continue
            categories = Counter(
                int(region.get("internal_detector_category_index", -1))
                for region in regions
                if int(region.get("internal_detector_category_index", -1)) >= 0
            )
            appearances.append({
                "north_star_id": reference["id"],
                "source_track_id": track["source_track_id"],
                "sequence_index": sequence_index,
                "output_track_id": output_track_id,
                "start_us": int(track.get("start_us", 0)),
                "end_us": int(track.get("end_us", 0)),
                "embeddings": embeddings,
                "categories": sorted(categories),
                "mean_screen_area": (
                    sum(float(region["screen_area_ratio"]) for region in regions) /
                    len(regions)
                ),
            })
    return appearances


def _pair(
    left: dict[str, object],
    right: dict[str, object],
    same_identity: bool,
    minimum_observations: int,
) -> dict[str, object]:
    left_area = float(left["mean_screen_area"])
    right_area = float(right["mean_screen_area"])
    larger_area = max(left_area, right_area)
    return {
        "same_identity": same_identity,
        "left_north_star_id": left["north_star_id"],
        "left_source_track_id": left["source_track_id"],
        "right_north_star_id": right["north_star_id"],
        "right_source_track_id": right["source_track_id"],
        "appearance_consensus": bidirectional_consensus(
            left["embeddings"], right["embeddings"], minimum_observations
        ),
        "category_overlap": bool(
            set(left["categories"]) & set(right["categories"])
        ),
        "area_similarity": (
            min(left_area, right_area) / larger_area
            if larger_area > 0.0 else 0.0
        ),
    }


def _threshold_result(
    positives: list[dict[str, object]],
    negatives: list[dict[str, object]],
    threshold: float,
) -> dict[str, object]:
    true_positives = sum(
        float(pair["appearance_consensus"]) >= threshold for pair in positives
    )
    false_positives = sum(
        float(pair["appearance_consensus"]) >= threshold for pair in negatives
    )
    true_positive_rate = true_positives / len(positives) if positives else 0.0
    false_positive_rate = false_positives / len(negatives) if negatives else 0.0
    accepted = true_positives + false_positives
    return {
        "threshold": threshold,
        "true_positive_count": true_positives,
        "false_positive_count": false_positives,
        "true_positive_rate": true_positive_rate,
        "false_positive_rate": false_positive_rate,
        "precision": true_positives / accepted if accepted else 1.0,
        "balanced_accuracy": (true_positive_rate + 1.0 - false_positive_rate) / 2.0,
    }


def _retrieval_evaluation(
    appearances: list[dict[str, object]],
    minimum_observations: int,
    thresholds: tuple[float, ...],
    margins: tuple[float, ...],
) -> dict[str, object]:
    queries = []
    chronological = sorted(
        appearances,
        key=lambda item: (
            item["start_us"], item["end_us"], item["north_star_id"],
            item["source_track_id"],
        ),
    )
    for current_index, current in enumerate(chronological):
        gallery = [
            previous for previous in chronological[:current_index]
            if int(previous["end_us"]) < int(current["start_us"])
        ]
        if not any(
            previous["north_star_id"] == current["north_star_id"]
            for previous in gallery
        ):
            continue
        ranked = sorted(
            (
                (
                    bidirectional_consensus(
                        previous["embeddings"], current["embeddings"],
                        minimum_observations,
                    ),
                    previous,
                )
                for previous in gallery
            ),
            key=lambda item: (
                -item[0], item[1]["north_star_id"],
                item[1]["source_track_id"],
            ),
        )
        best_score, best = ranked[0]
        second_score = ranked[1][0] if len(ranked) > 1 else -1.0
        queries.append({
            "north_star_id": current["north_star_id"],
            "source_track_id": current["source_track_id"],
            "best_north_star_id": best["north_star_id"],
            "best_source_track_id": best["source_track_id"],
            "best_score": best_score,
            "second_best_score": second_score,
            "margin": best_score - second_score,
            "correct": best["north_star_id"] == current["north_star_id"],
        })

    policies = []
    for threshold in thresholds:
        for margin in margins:
            accepted = [
                query for query in queries
                if float(query["best_score"]) >= threshold and
                float(query["margin"]) >= margin
            ]
            correct = sum(bool(query["correct"]) for query in accepted)
            policies.append({
                "threshold": threshold,
                "minimum_margin": margin,
                "accepted_count": len(accepted),
                "correct_count": correct,
                "precision": correct / len(accepted) if accepted else 1.0,
                "recall": correct / len(queries) if queries else 0.0,
            })
    return {
        "summary": {
            "query_count": len(queries),
            "top1_correct_count": sum(
                bool(query["correct"]) for query in queries
            ),
            "top1_accuracy": (
                sum(bool(query["correct"]) for query in queries) / len(queries)
                if queries else 0.0
            ),
        },
        "policies": policies,
        "queries": queries,
    }


def evaluate(
    diagnostic: dict[str, object],
    benchmark: dict[str, object],
    minimum_observations: int = 2,
    thresholds: tuple[float, ...] = DEFAULT_THRESHOLDS,
    margins: tuple[float, ...] = DEFAULT_MARGINS,
) -> dict[str, object]:
    if minimum_observations < 1:
        raise ValueError("minimum observations must be positive")
    if not thresholds or any(not 0.0 < threshold <= 1.0 for threshold in thresholds):
        raise ValueError("thresholds must be in (0, 1]")
    if not margins or any(margin < 0.0 for margin in margins):
        raise ValueError("margins must be non-negative")

    appearances = _track_appearances(
        diagnostic, benchmark, minimum_observations
    )
    by_identity: dict[str, list[dict[str, object]]] = defaultdict(list)
    for appearance in appearances:
        by_identity[str(appearance["north_star_id"])].append(appearance)
    for identity_appearances in by_identity.values():
        identity_appearances.sort(
            key=lambda item: (item["sequence_index"], item["source_track_id"])
        )

    positives = []
    for identity_appearances in by_identity.values():
        positives.extend(
            _pair(left, right, True, minimum_observations)
            for left, right in zip(
                identity_appearances, identity_appearances[1:]
            )
        )

    negatives = []
    ordered_appearances = sorted(
        appearances,
        key=lambda item: (
            item["north_star_id"], item["sequence_index"], item["source_track_id"]
        ),
    )
    for index, left in enumerate(ordered_appearances):
        for right in ordered_appearances[index + 1:]:
            if left["north_star_id"] == right["north_star_id"]:
                continue
            negatives.append(_pair(left, right, False, minimum_observations))

    threshold_results = [
        _threshold_result(positives, negatives, threshold)
        for threshold in thresholds
    ]
    best = min(
        threshold_results,
        key=lambda result: (
            -float(result["balanced_accuracy"]),
            float(result["false_positive_rate"]),
            -float(result["threshold"]),
        ),
    )
    return {
        "policy": {
            "minimum_observations": minimum_observations,
            "thresholds": list(thresholds),
            "margins": list(margins),
        },
        "summary": {
            "appearance_count": len(appearances),
            "positive_pair_count": len(positives),
            "negative_pair_count": len(negatives),
            "best_threshold": best["threshold"],
            "best_balanced_accuracy": best["balanced_accuracy"],
            "best_true_positive_rate": best["true_positive_rate"],
            "best_false_positive_rate": best["false_positive_rate"],
        },
        "threshold_results": threshold_results,
        "positive_pairs": positives,
        "negative_pairs": negatives,
        "retrieval": _retrieval_evaluation(
            appearances, minimum_observations, thresholds, margins
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("diagnostic", type=Path)
    parser.add_argument("benchmark", type=Path)
    parser.add_argument("--minimum-observations", type=int, default=2)
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    result = evaluate(
        json.loads(arguments.diagnostic.read_text()),
        json.loads(arguments.benchmark.read_text()),
        arguments.minimum_observations,
    )
    rendered = json.dumps(result, indent=2) + "\n"
    if arguments.output:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(rendered)
    else:
        print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
