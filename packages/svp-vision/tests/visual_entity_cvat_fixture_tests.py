#!/usr/bin/env python3

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


TOOL_PATH = (
    Path(__file__).parents[1] / "tools" / "cvat_visual_entity_fixture.py"
)
SPEC = importlib.util.spec_from_file_location("cvat_fixture", TOOL_PATH)
CVAT_FIXTURE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = CVAT_FIXTURE
SPEC.loader.exec_module(CVAT_FIXTURE)


def annotation_xml(*tracks: str) -> str:
    return f"""<?xml version="1.0" encoding="utf-8"?>
<annotations>
  <version>1.1</version>
  <meta>
    <job><size>10</size></job>
    <original_size><width>100</width><height>50</height></original_size>
  </meta>
  {''.join(tracks)}
</annotations>
"""


def track(track_id: int, north_star_id: str, start: int, end: int) -> str:
    boxes = []
    for frame in range(start, end + 1):
        boxes.append(f"""
    <box frame="{frame}" keyframe="1" outside="0" occluded="0"
         xtl="10" ytl="5" xbr="60" ybr="30" z_order="0">
      <attribute name="north_star_id">{north_star_id}</attribute>
      <attribute name="kind">persistent_entity</attribute>
      <attribute name="notes"></attribute>
    </box>""")
    boxes.append(f"""
    <box frame="{end + 1}" keyframe="1" outside="1" occluded="0"
         xtl="10" ytl="5" xbr="60" ybr="30" z_order="0">
      <attribute name="north_star_id">{north_star_id}</attribute>
      <attribute name="kind">persistent_entity</attribute>
      <attribute name="notes"></attribute>
    </box>""")
    return f'<track id="{track_id}" label="entity">{"".join(boxes)}</track>'


class CvatVisualEntityFixtureTests(unittest.TestCase):
    def convert(self, xml: str):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "annotations.xml"
            source.write_text(xml)
            return CVAT_FIXTURE.convert(
                source,
                "sample.mp4",
                CVAT_FIXTURE.FrameRate.parse("10/1"),
                1_000_000,
                200_000,
            )

    def test_groups_separate_tracks_under_one_semantic_entity(self):
        fixture = self.convert(annotation_xml(
            track(7, "controller", 0, 2),
            track(9, "controller", 5, 7),
        ))

        video = fixture["sample.mp4"]
        self.assertEqual(1, len(video["references"]))
        reference = video["references"][0]
        self.assertEqual("controller", reference["id"])
        self.assertEqual(2, len(reference["tracks"]))
        self.assertEqual([[0, 300_000], [500_000, 800_000]], reference["intervals"])
        self.assertEqual(
            {"7", "9"},
            {point["source_track_id"] for point in reference["checkpoints"]},
        )

    def test_normalizes_proxy_coordinates(self):
        fixture = self.convert(annotation_xml(track(1, "box", 0, 2)))
        checkpoint = fixture["sample.mp4"]["references"][0]["checkpoints"][0]
        self.assertEqual([0.1, 0.1, 0.6, 0.6], checkpoint["box_norm"])

    def test_rejects_missing_identity(self):
        with self.assertRaisesRegex(ValueError, "has no north_star_id"):
            self.convert(annotation_xml(track(1, "", 0, 2)))


if __name__ == "__main__":
    unittest.main()
