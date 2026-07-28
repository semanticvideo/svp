#!/usr/bin/env python3
"""Score visual-entity diagnostic output against a track-aware North Star."""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
from typing import Iterable


def box_iou(left: list[float], right: list[float]) -> float:
    x0 = max(left[0], right[0])
    y0 = max(left[1], right[1])
    x1 = min(left[2], right[2])
    y1 = min(left[3], right[3])
    intersection = max(0.0, x1 - x0) * max(0.0, y1 - y0)
    left_area = max(0.0, left[2] - left[0]) * max(0.0, left[3] - left[1])
    right_area = max(0.0, right[2] - right[0]) * max(0.0, right[3] - right[1])
    union = left_area + right_area - intersection
    return intersection / union if union > 0.0 else 0.0


def _dominant(values: Iterable[str]) -> tuple[str, int]:
    counts = Counter(value for value in values if value)
    if not counts:
        return "", 0
    value, count = min(counts.items(), key=lambda entry: (-entry[1], entry[0]))
    return value, count


def _candidate_accepts_kind(kind: str, region: dict[str, object]) -> bool:
    source = region.get("candidate_source", "")
    if kind == "dynamic_group":
        return source == "motion_group"
    if kind == "persistent_entity":
        return source != "motion_group"
    return True


def score(
    diagnostic: dict[str, object],
    fixture: dict[str, object],
    video_name: str,
    minimum_match_iou: float = 0.30,
    timestamp_tolerance_us: int = 100_000,
    diagnostic_timestamp_offset_us: int = 0,
    evaluation_start_us: int | None = None,
    evaluation_end_us: int | None = None,
) -> dict[str, object]:
    if not 0.0 < minimum_match_iou <= 1.0:
        raise ValueError("minimum match IoU must be in (0, 1]")
    video = fixture.get(video_name)
    if not video:
        raise ValueError(f"north star has no entry for {video_name}")

    evaluation_start_us = 0 if evaluation_start_us is None else evaluation_start_us
    evaluation_end_us = (
        int(video.get("duration_us", 2**63 - 1))
        if evaluation_end_us is None else evaluation_end_us
    )
    if evaluation_start_us < 0 or evaluation_end_us <= evaluation_start_us:
        raise ValueError("evaluation time range is invalid")

    evaluated_references = []
    for reference in video["references"]:
        checkpoints = [
            checkpoint for checkpoint in reference["checkpoints"]
            if evaluation_start_us <= int(checkpoint["pts_us"]) < evaluation_end_us
        ]
        source_track_ids = {
            checkpoint["source_track_id"] for checkpoint in checkpoints
        }
        tracks = [
            track for track in reference["tracks"]
            if track["source_track_id"] in source_track_ids
        ]
        if checkpoints:
            evaluated_references.append({
                **reference,
                "tracks": tracks,
                "checkpoints": checkpoints,
            })

    expected_adjacent_boundaries_us = set()
    for reference in evaluated_references:
        tracks = sorted(
            reference["tracks"],
            key=lambda track: (track["start_frame"], track["source_track_id"]),
        )
        for previous, current in zip(tracks, tracks[1:]):
            if current["start_frame"] == previous["end_frame"] + 1:
                expected_adjacent_boundaries_us.add(int(current["start_us"]))
    detected_cut_timestamps_us = [
        int(evidence["timestamp_us"]) + diagnostic_timestamp_offset_us
        for evidence in diagnostic.get("cut_evidence", [])
        if evidence.get("is_cut")
    ]
    detected_adjacent_boundaries = sum(
        any(abs(cut - boundary) <= timestamp_tolerance_us
            for cut in detected_cut_timestamps_us)
        for boundary in expected_adjacent_boundaries_us
    )

    regions = []
    for source_region in diagnostic.get("regions", []):
        region = dict(source_region)
        region["pts_us"] = (
            int(region["pts_us"]) + diagnostic_timestamp_offset_us
        )
        regions.append(region)
    regions_by_time: dict[int, list[dict[str, object]]] = defaultdict(list)
    for region in regions:
        if "track_id" not in region:
            raise ValueError(
                "diagnostic regions must include track_id for track-aware scoring"
            )
        regions_by_time[int(region["pts_us"])].append(region)
    output_times = sorted(regions_by_time)

    def nearby_regions(timestamp_us: int) -> Iterable[dict[str, object]]:
        for output_time in output_times:
            if output_time < timestamp_us - timestamp_tolerance_us:
                continue
            if output_time > timestamp_us + timestamp_tolerance_us:
                break
            yield from regions_by_time[output_time]

    # Match all references at a sampled timestamp together. A region may
    # explain at most one human checkpoint in that sample.
    checkpoint_matches: dict[tuple[str, str, int], tuple[float, dict[str, object]]] = {}
    checkpoint_groups: dict[int, list[tuple[str, str, dict[str, object]]]] = (
        defaultdict(list)
    )
    for reference in evaluated_references:
        for checkpoint in reference["checkpoints"]:
            checkpoint_groups[int(checkpoint["pts_us"])].append(
                (reference["id"], reference["kind"], checkpoint)
            )
    for timestamp_us, expected in checkpoint_groups.items():
        candidates = list(nearby_regions(timestamp_us))
        edges = []
        for reference_id, kind, checkpoint in expected:
            for region_index, region in enumerate(candidates):
                if not _candidate_accepts_kind(kind, region):
                    continue
                iou = box_iou(checkpoint["box_norm"], region["box_norm"])
                edges.append((
                    -iou,
                    reference_id,
                    str(checkpoint["source_track_id"]),
                    str(region["entity_id"]),
                    str(region["track_id"]),
                    region_index,
                    checkpoint,
                    region,
                ))
        assigned_checkpoints: set[tuple[str, str, int]] = set()
        assigned_regions: set[int] = set()
        for edge in sorted(edges, key=lambda item: item[:6]):
            iou = -edge[0]
            reference_id = edge[1]
            checkpoint = edge[6]
            region_index = edge[5]
            region = edge[7]
            key = (
                reference_id,
                str(checkpoint["source_track_id"]),
                int(checkpoint["pts_us"]),
            )
            if key in assigned_checkpoints or region_index in assigned_regions:
                continue
            assigned_checkpoints.add(key)
            assigned_regions.add(region_index)
            checkpoint_matches[key] = (iou, region)

    checkpoint_results: list[dict[str, object]] = []
    checkpoint_by_source_track: dict[str, list[dict[str, object]]] = defaultdict(list)
    expected_by_time: dict[int, list[dict[str, object]]] = defaultdict(list)
    reference_kind_by_track: dict[str, str] = {}

    for reference in evaluated_references:
        kind = reference["kind"]
        for track in reference["tracks"]:
            reference_kind_by_track[track["source_track_id"]] = kind
        for checkpoint in reference["checkpoints"]:
            expected_by_time[int(checkpoint["pts_us"])].append(checkpoint)
            match = checkpoint_matches.get((
                reference["id"],
                str(checkpoint["source_track_id"]),
                int(checkpoint["pts_us"]),
            ))
            best_iou = match[0] if match else 0.0
            best_region = match[1] if match else None
            matched = best_region is not None and best_iou >= minimum_match_iou
            result = {
                "north_star_id": reference["id"],
                "source_track_id": checkpoint["source_track_id"],
                "pts_us": checkpoint["pts_us"],
                "iou": best_iou,
                "matched": matched,
                "output_entity_id": best_region["entity_id"] if matched else "",
                "output_track_id": best_region["track_id"] if matched else "",
            }
            checkpoint_results.append(result)
            checkpoint_by_source_track[checkpoint["source_track_id"]].append(result)

    reference_results: list[dict[str, object]] = []
    source_track_results: list[dict[str, object]] = []
    dominant_entity_to_references: dict[str, set[str]] = defaultdict(set)
    total_breaks = 0
    correct_breaks = 0
    identity_transitions = 0
    correct_identity_transitions = 0
    correct_entity_and_track_transitions = 0
    repeated_identity_count = 0
    consistent_identity_count = 0

    for reference in evaluated_references:
        track_results: list[dict[str, object]] = []
        for source_track in reference["tracks"]:
            points = checkpoint_by_source_track[source_track["source_track_id"]]
            matched = [point for point in points if point["matched"]]
            entity_id, entity_count = _dominant(
                point["output_entity_id"] for point in matched
            )
            track_id, track_count = _dominant(
                point["output_track_id"] for point in matched
            )
            if entity_id:
                dominant_entity_to_references[entity_id].add(reference["id"])
            result = {
                **source_track,
                "checkpoint_count": len(points),
                "matched_checkpoint_count": len(matched),
                "detection_recall": len(matched) / len(points) if points else 0.0,
                "mean_checkpoint_iou": (
                    sum(point["iou"] for point in points) / len(points)
                    if points else 0.0
                ),
                "dominant_output_entity_id": entity_id,
                "dominant_output_track_id": track_id,
                "entity_match_purity": (
                    entity_count / len(matched) if matched else 0.0
                ),
                "track_match_purity": (
                    track_count / len(matched) if matched else 0.0
                ),
                "matched_output_track_count": len({
                    point["output_track_id"] for point in matched
                }),
            }
            track_results.append(result)
            source_track_results.append(result)

        mapped_entities = [
            track["dominant_output_entity_id"]
            for track in track_results
            if track["dominant_output_entity_id"]
        ]
        identity_consistent = True
        if len(mapped_entities) > 1:
            repeated_identity_count += 1
            identity_consistent = len(set(mapped_entities)) == 1
            if identity_consistent:
                consistent_identity_count += 1

        break_count = 0
        correct_break_count = 0
        identity_transition_count = 0
        correct_identity_transition_count = 0
        correct_entity_and_track_transition_count = 0
        for previous, current in zip(track_results, track_results[1:]):
            previous_entity = previous["dominant_output_entity_id"]
            current_entity = current["dominant_output_entity_id"]
            previous_track = previous["dominant_output_track_id"]
            current_track = current["dominant_output_track_id"]
            if (not previous_entity or not current_entity or
                    not previous_track or not current_track):
                continue
            identity_transition_count += 1
            break_count += 1
            same_entity = previous_entity == current_entity
            distinct_track = previous_track != current_track
            if same_entity:
                correct_identity_transition_count += 1
            if same_entity and distinct_track:
                correct_entity_and_track_transition_count += 1
            if distinct_track:
                correct_break_count += 1
        identity_transitions += identity_transition_count
        correct_identity_transitions += correct_identity_transition_count
        correct_entity_and_track_transitions += (
            correct_entity_and_track_transition_count
        )
        total_breaks += break_count
        correct_breaks += correct_break_count
        reference_results.append({
            "id": reference["id"],
            "kind": reference["kind"],
            "source_track_count": len(track_results),
            "mapped_source_track_count": len(mapped_entities),
            "identity_consistent_across_appearances": identity_consistent,
            "track_break_count": break_count,
            "correct_track_break_count": correct_break_count,
            "track_break_accuracy": (
                correct_break_count / break_count if break_count else 1.0
            ),
            "identity_transition_accuracy": (
                correct_identity_transition_count / identity_transition_count
                if identity_transition_count else 1.0
            ),
            "identity_transition_count": identity_transition_count,
            "correct_identity_transition_count": (
                correct_identity_transition_count
            ),
            "correct_entity_and_track_transition_count": (
                correct_entity_and_track_transition_count
            ),
            "correct_entity_and_track_transition_accuracy": (
                correct_entity_and_track_transition_count /
                identity_transition_count
                if identity_transition_count else 1.0
            ),
            "tracks": track_results,
        })

    matched_checkpoints = [point for point in checkpoint_results if point["matched"]]
    cross_identity_collisions = {
        entity_id: sorted(reference_ids)
        for entity_id, reference_ids in dominant_entity_to_references.items()
        if len(reference_ids) > 1
    }

    unmatched_output_regions = 0
    comparable_output_regions = 0
    for output_time, output_regions in regions_by_time.items():
        nearest_expected: list[dict[str, object]] = []
        best_delta = timestamp_tolerance_us + 1
        for expected_time, expected in expected_by_time.items():
            delta = abs(expected_time - output_time)
            if delta <= timestamp_tolerance_us and delta < best_delta:
                best_delta = delta
                nearest_expected = expected
        if not nearest_expected:
            continue
        for region in output_regions:
            comparable_output_regions += 1
            if max(
                (box_iou(point["box_norm"], region["box_norm"])
                 for point in nearest_expected),
                default=0.0,
            ) < minimum_match_iou:
                unmatched_output_regions += 1

    return {
        "video": video_name,
        "policy": {
            "minimum_match_iou": minimum_match_iou,
            "timestamp_tolerance_us": timestamp_tolerance_us,
            "diagnostic_timestamp_offset_us": diagnostic_timestamp_offset_us,
            "evaluation_start_us": evaluation_start_us,
            "evaluation_end_us": evaluation_end_us,
        },
        "summary": {
            "reference_identity_count": len(evaluated_references),
            "reference_track_count": len(source_track_results),
            "reference_checkpoint_count": len(checkpoint_results),
            "output_entity_count": len(diagnostic.get("entities", [])),
            "output_track_count": len(diagnostic.get("tracks", [])),
            "output_region_count": len(regions),
            "checkpoint_detection_recall": (
                len(matched_checkpoints) / len(checkpoint_results)
                if checkpoint_results else 0.0
            ),
            "mean_checkpoint_iou": (
                sum(point["iou"] for point in checkpoint_results) /
                len(checkpoint_results)
                if checkpoint_results else 0.0
            ),
            "mean_source_track_detection_recall": (
                sum(track["detection_recall"] for track in source_track_results) /
                len(source_track_results)
                if source_track_results else 0.0
            ),
            "repeated_identity_consistency": (
                consistent_identity_count / repeated_identity_count
                if repeated_identity_count else 1.0
            ),
            "track_break_accuracy": (
                correct_breaks / total_breaks if total_breaks else 1.0
            ),
            "comparable_appearance_transition_count": identity_transitions,
            "correct_identity_transition_count": correct_identity_transitions,
            "correct_entity_and_track_transition_count": (
                correct_entity_and_track_transitions
            ),
            "identity_transition_accuracy": (
                correct_identity_transitions / identity_transitions
                if identity_transitions else 1.0
            ),
            "correct_entity_and_track_transition_accuracy": (
                correct_entity_and_track_transitions / identity_transitions
                if identity_transitions else 1.0
            ),
            "cross_identity_collision_count": len(cross_identity_collisions),
            "expected_adjacent_track_boundary_count": (
                len(expected_adjacent_boundaries_us)
            ),
            "detected_adjacent_track_boundary_count": (
                detected_adjacent_boundaries
            ),
            "adjacent_track_boundary_detection_recall": (
                detected_adjacent_boundaries /
                len(expected_adjacent_boundaries_us)
                if expected_adjacent_boundaries_us else 1.0
            ),
            "comparable_output_region_count": comparable_output_regions,
            "unmatched_output_region_count": unmatched_output_regions,
            "unmatched_output_region_rate": (
                unmatched_output_regions / comparable_output_regions
                if comparable_output_regions else 0.0
            ),
        },
        "cross_identity_collisions": cross_identity_collisions,
        "references": reference_results,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("diagnostic", type=Path)
    parser.add_argument("north_star", type=Path)
    parser.add_argument("video_name")
    parser.add_argument("--minimum-match-iou", type=float, default=0.30)
    parser.add_argument("--timestamp-tolerance-us", type=int, default=100_000)
    parser.add_argument("--diagnostic-timestamp-offset-us", type=int, default=0)
    parser.add_argument("--evaluation-start-us", type=int)
    parser.add_argument("--evaluation-end-us", type=int)
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    result = score(
        json.loads(arguments.diagnostic.read_text()),
        json.loads(arguments.north_star.read_text()),
        arguments.video_name,
        arguments.minimum_match_iou,
        arguments.timestamp_tolerance_us,
        arguments.diagnostic_timestamp_offset_us,
        arguments.evaluation_start_us,
        arguments.evaluation_end_us,
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
