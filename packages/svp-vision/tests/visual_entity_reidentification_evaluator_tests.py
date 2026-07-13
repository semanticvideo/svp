#!/usr/bin/env python3

import importlib.util
from pathlib import Path
import sys
import unittest


TOOL_PATH = (
    Path(__file__).parents[1] /
    "tools" /
    "visual_entity_reidentification_evaluator.py"
)
SPEC = importlib.util.spec_from_file_location("reidentification_evaluator", TOOL_PATH)
EVALUATOR = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = EVALUATOR
SPEC.loader.exec_module(EVALUATOR)


def region(track_id, embedding, category=1, area=0.2, pts_us=None):
    value = {
        "track_id": track_id,
        "embedding": embedding,
        "internal_detector_category_index": category,
        "screen_area_ratio": area,
    }
    if pts_us is not None:
        value["pts_us"] = pts_us
    return value


def reference(reference_id, tracks):
    return {
        "id": reference_id,
        "tracks": [
            {
                "source_track_id": track[0],
                "dominant_output_track_id": track[1],
                **(
                    {"start_us": track[2], "end_us": track[3]}
                    if len(track) == 4 else {}
                ),
            }
            for track in tracks
        ],
    }


class VisualEntityReidentificationEvaluatorTests(unittest.TestCase):
    def test_separates_repeated_identity_from_distinct_identity(self):
        diagnostic = {
            "regions": [
                region("a1", [1.0, 0.0]),
                region("a1", [0.99, 0.01]),
                region("a2", [1.0, 0.0]),
                region("a2", [0.99, 0.01]),
                region("b1", [0.0, 1.0]),
                region("b1", [0.01, 0.99]),
            ]
        }
        benchmark = {
            "references": [
                reference("controller", [("1", "a1"), ("2", "a2")]),
                reference("phone", [("3", "b1")]),
            ]
        }

        result = EVALUATOR.evaluate(
            diagnostic, benchmark, thresholds=(0.90,)
        )

        self.assertEqual(1, result["summary"]["positive_pair_count"])
        self.assertEqual(2, result["summary"]["negative_pair_count"])
        threshold = result["threshold_results"][0]
        self.assertEqual(1.0, threshold["true_positive_rate"])
        self.assertEqual(0.0, threshold["false_positive_rate"])

    def test_requires_repeated_observations_on_each_appearance(self):
        diagnostic = {
            "regions": [
                region("a1", [1.0, 0.0]),
                region("a1", [0.99, 0.01]),
                region("a2", [1.0, 0.0]),
            ]
        }
        benchmark = {
            "references": [
                reference("controller", [("1", "a1"), ("2", "a2")])
            ]
        }

        result = EVALUATOR.evaluate(diagnostic, benchmark)

        self.assertEqual(1, result["summary"]["appearance_count"])
        self.assertEqual(0, result["summary"]["positive_pair_count"])

    def test_limits_embeddings_to_the_annotated_track_interval(self):
        diagnostic = {
            "regions": [
                region("shared", [1.0, 0.0], pts_us=10),
                region("shared", [0.99, 0.01], pts_us=20),
                region("shared", [0.0, 1.0], pts_us=110),
                region("shared", [0.01, 0.99], pts_us=120),
            ]
        }
        benchmark = {
            "policy": {"diagnostic_timestamp_offset_us": 1000},
            "references": [
                reference(
                    "controller",
                    [("1", "shared", 1010, 1020),
                     ("2", "shared", 1110, 1120)],
                )
            ],
        }

        result = EVALUATOR.evaluate(
            diagnostic, benchmark, thresholds=(0.90,)
        )

        self.assertEqual(1, result["summary"]["positive_pair_count"])
        self.assertEqual(
            0.0, result["threshold_results"][0]["true_positive_rate"]
        )

    def test_rejects_invalid_thresholds(self):
        with self.assertRaisesRegex(ValueError, "thresholds"):
            EVALUATOR.evaluate({}, {}, thresholds=(0.0,))

    def test_rejects_invalid_margins(self):
        with self.assertRaisesRegex(ValueError, "margins"):
            EVALUATOR.evaluate({}, {}, margins=(-0.01,))

    def test_retrieval_reports_ambiguity_margin(self):
        diagnostic = {
            "regions": [
                region("a1", [1.0, 0.0], pts_us=10),
                region("a1", [0.99, 0.01], pts_us=20),
                region("b1", [0.0, 1.0], pts_us=10),
                region("b1", [0.01, 0.99], pts_us=20),
                region("a2", [0.98, 0.02], pts_us=310),
                region("a2", [1.0, 0.0], pts_us=320),
            ]
        }
        benchmark = {
            "references": [
                reference("controller", [
                    ("1", "a1", 0, 100), ("2", "a2", 300, 400)
                ]),
                reference("phone", [("3", "b1", 0, 100)]),
            ]
        }

        result = EVALUATOR.evaluate(
            diagnostic, benchmark, thresholds=(0.90,), margins=(0.05,)
        )

        retrieval = result["retrieval"]
        self.assertEqual(1, retrieval["summary"]["query_count"])
        self.assertEqual(1.0, retrieval["summary"]["top1_accuracy"])
        self.assertEqual(1, retrieval["policies"][0]["accepted_count"])


if __name__ == "__main__":
    unittest.main()
