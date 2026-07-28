#!/usr/bin/env python3

import importlib.util
from pathlib import Path
import sys
import unittest


TOOL_PATH = (
    Path(__file__).parents[1] / "tools" / "visual_entity_benchmark_score.py"
)
SPEC = importlib.util.spec_from_file_location("benchmark_score", TOOL_PATH)
BENCHMARK = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = BENCHMARK
SPEC.loader.exec_module(BENCHMARK)


def reference(reference_id, source_tracks):
    tracks = []
    checkpoints = []
    for index, source_track_id in enumerate(source_tracks):
        timestamp = index * 200_000
        tracks.append({
            "source_track_id": source_track_id,
            "start_frame": index,
            "end_frame": index,
            "start_us": timestamp,
            "end_us": timestamp + 200_000,
        })
        checkpoints.append({
            "source_track_id": source_track_id,
            "pts_us": timestamp,
            "box_norm": [0.1, 0.1, 0.4, 0.4],
        })
    return {
        "id": reference_id,
        "kind": "persistent_entity",
        "tracks": tracks,
        "checkpoints": checkpoints,
    }


def region(timestamp, entity_id, track_id, box=None):
    return {
        "pts_us": timestamp,
        "entity_id": entity_id,
        "track_id": track_id,
        "candidate_source": "objectness_detector",
        "box_norm": box or [0.1, 0.1, 0.4, 0.4],
    }


class VisualEntityBenchmarkScoreTests(unittest.TestCase):
    def score(self, references, regions, tracks=None):
        diagnostic = {
            "entities": [{"id": "entity_1"}],
            "tracks": tracks or [],
            "regions": regions,
        }
        fixture = {"sample.mp4": {"references": references}}
        return BENCHMARK.score(diagnostic, fixture, "sample.mp4")

    def test_rewards_one_entity_with_distinct_appearance_tracks(self):
        result = self.score(
            [reference("controller", ["7", "9"])],
            [
                region(0, "entity_1", "track_1"),
                region(200_000, "entity_1", "track_2"),
            ],
        )

        summary = result["summary"]
        self.assertEqual(1.0, summary["checkpoint_detection_recall"])
        self.assertEqual(1.0, summary["repeated_identity_consistency"])
        self.assertEqual(1.0, summary["track_break_accuracy"])
        self.assertEqual(1.0, summary["identity_transition_accuracy"])
        self.assertEqual(
            1.0, summary["correct_entity_and_track_transition_accuracy"]
        )

    def test_rejects_one_output_track_spanning_two_human_tracks(self):
        result = self.score(
            [reference("controller", ["7", "9"])],
            [
                region(0, "entity_1", "track_1"),
                region(200_000, "entity_1", "track_1"),
            ],
        )

        self.assertEqual(0.0, result["summary"]["track_break_accuracy"])
        self.assertEqual(1.0, result["summary"]["identity_transition_accuracy"])
        self.assertEqual(
            0.0,
            result["summary"]["correct_entity_and_track_transition_accuracy"],
        )

    def test_reports_cross_identity_collisions(self):
        result = self.score(
            [reference("controller", ["7"]), reference("phone", ["9"])],
            [
                region(0, "entity_1", "track_1"),
                region(0, "entity_1", "track_1"),
            ],
        )

        self.assertEqual(1, result["summary"]["cross_identity_collision_count"])

    def test_one_output_region_cannot_match_two_references(self):
        result = self.score(
            [reference("controller", ["7"]), reference("phone", ["9"])],
            [region(0, "entity_1", "track_1")],
        )

        self.assertEqual(
            0.5, result["summary"]["checkpoint_detection_recall"]
        )

    def test_does_not_reward_a_track_break_when_identity_was_lost(self):
        result = self.score(
            [reference("controller", ["7", "9"])],
            [
                region(0, "entity_1", "track_1"),
                region(200_000, "entity_2", "track_2"),
            ],
        )

        summary = result["summary"]
        self.assertEqual(1.0, summary["track_break_accuracy"])
        self.assertEqual(0.0, summary["identity_transition_accuracy"])
        self.assertEqual(
            0.0, summary["correct_entity_and_track_transition_accuracy"]
        )

    def test_requires_region_track_identity(self):
        with self.assertRaisesRegex(ValueError, "must include track_id"):
            self.score(
                [reference("controller", ["7"])],
                [{
                    "pts_us": 0,
                    "entity_id": "entity_1",
                    "candidate_source": "objectness_detector",
                    "box_norm": [0.1, 0.1, 0.4, 0.4],
                }],
            )

    def test_scores_a_time_shifted_proxy_slice(self):
        diagnostic = {
            "entities": [{"id": "entity_1"}],
            "tracks": [{"id": "track_1"}],
            "regions": [region(0, "entity_1", "track_1")],
        }
        fixture = {
            "sample.mp4": {
                "duration_us": 400_000,
                "references": [reference("controller", ["7", "9"])],
            }
        }

        result = BENCHMARK.score(
            diagnostic,
            fixture,
            "sample.mp4",
            diagnostic_timestamp_offset_us=200_000,
            evaluation_start_us=200_000,
            evaluation_end_us=400_000,
        )

        self.assertEqual(1, result["summary"]["reference_track_count"])
        self.assertEqual(1.0, result["summary"]["checkpoint_detection_recall"])

    def test_scores_detected_adjacent_human_track_boundaries(self):
        diagnostic = {
            "entities": [{"id": "entity_1"}],
            "tracks": [{"id": "track_1"}, {"id": "track_2"}],
            "regions": [
                region(0, "entity_1", "track_1"),
                region(200_000, "entity_1", "track_2"),
            ],
            "cut_evidence": [{"timestamp_us": 200_000, "is_cut": True}],
        }
        fixture = {
            "sample.mp4": {
                "duration_us": 400_000,
                "references": [reference("controller", ["7", "9"])],
            }
        }

        result = BENCHMARK.score(diagnostic, fixture, "sample.mp4")

        self.assertEqual(
            1, result["summary"]["expected_adjacent_track_boundary_count"]
        )
        self.assertEqual(
            1.0,
            result["summary"]["adjacent_track_boundary_detection_recall"],
        )


if __name__ == "__main__":
    unittest.main()
