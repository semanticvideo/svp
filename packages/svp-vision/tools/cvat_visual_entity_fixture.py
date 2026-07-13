#!/usr/bin/env python3
"""Convert CVAT for video 1.1 tracks into an SVP visual-entity fixture."""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path
import zipfile
import xml.etree.ElementTree as ET


TRACKED_KINDS = {
    "persistent_entity",
    "dynamic_group",
    "graphic_region",
    "interface_region",
    "composite_region",
}


@dataclass(frozen=True)
class FrameRate:
    numerator: int
    denominator: int

    @classmethod
    def parse(cls, value: str) -> "FrameRate":
        rate = Fraction(value)
        if rate <= 0:
            raise ValueError("frame rate must be positive")
        return cls(rate.numerator, rate.denominator)

    def timestamp_us(self, frame: int) -> int:
        return round(
            frame * 1_000_000 * self.denominator / self.numerator
        )

    def nearest_frame(self, timestamp_us: int) -> int:
        return round(
            timestamp_us * self.numerator /
            (1_000_000 * self.denominator)
        )


def _load_root(path: Path) -> ET.Element:
    if path.suffix.lower() == ".zip":
        with zipfile.ZipFile(path) as archive:
            names = archive.namelist()
            if names.count("annotations.xml") != 1:
                raise ValueError(
                    "CVAT ZIP must contain exactly one annotations.xml"
                )
            with archive.open("annotations.xml") as source:
                return ET.parse(source).getroot()
    return ET.parse(path).getroot()


def _immutable_attributes(boxes: list[ET.Element]) -> dict[str, str]:
    values: dict[str, set[str]] = {}
    for box in boxes:
        for attribute in box.findall("attribute"):
            name = attribute.attrib["name"]
            values.setdefault(name, set()).add(attribute.text or "")
    conflicts = sorted(name for name, entries in values.items() if len(entries) != 1)
    if conflicts:
        raise ValueError(
            "immutable CVAT attributes changed inside a track: "
            + ", ".join(conflicts)
        )
    return {name: next(iter(entries)) for name, entries in values.items()}


def _visible_runs(boxes: list[ET.Element]) -> list[list[ET.Element]]:
    runs: list[list[ET.Element]] = []
    current: list[ET.Element] = []
    previous_frame: int | None = None
    for box in boxes:
        frame = int(box.attrib["frame"])
        visible = box.attrib["outside"] == "0"
        if visible and (previous_frame is None or frame == previous_frame + 1):
            current.append(box)
        elif visible:
            if current:
                runs.append(current)
            current = [box]
        elif current:
            runs.append(current)
            current = []
        previous_frame = frame
    if current:
        runs.append(current)
    return runs


def _normalized_box(box: ET.Element, width: int, height: int) -> list[float]:
    coordinates = [
        float(box.attrib["xtl"]) / width,
        float(box.attrib["ytl"]) / height,
        float(box.attrib["xbr"]) / width,
        float(box.attrib["ybr"]) / height,
    ]
    return [round(max(0.0, min(1.0, value)), 8) for value in coordinates]


def _sample_run(
    run: list[ET.Element],
    frame_rate: FrameRate,
    sample_interval_us: int,
    width: int,
    height: int,
    track_id: str,
) -> list[dict[str, object]]:
    boxes_by_frame = {int(box.attrib["frame"]): box for box in run}
    start_frame = min(boxes_by_frame)
    end_frame = max(boxes_by_frame)
    start_us = frame_rate.timestamp_us(start_frame)
    end_us = frame_rate.timestamp_us(end_frame + 1)
    first_sample_us = (
        (start_us + sample_interval_us - 1) // sample_interval_us
    ) * sample_interval_us
    checkpoints: list[dict[str, object]] = []
    for timestamp_us in range(first_sample_us, end_us, sample_interval_us):
        nearest = max(
            start_frame,
            min(end_frame, frame_rate.nearest_frame(timestamp_us)),
        )
        if nearest not in boxes_by_frame:
            nearest = min(
                boxes_by_frame,
                key=lambda frame: (abs(frame - nearest), frame),
            )
        box = boxes_by_frame[nearest]
        checkpoints.append({
            "pts_us": timestamp_us,
            "source_frame": nearest,
            "source_track_id": track_id,
            "box_norm": _normalized_box(box, width, height),
            "occluded": box.attrib["occluded"] == "1",
        })
    return checkpoints


def convert(
    source: Path,
    video_name: str,
    frame_rate: FrameRate,
    duration_us: int,
    sample_interval_us: int,
) -> dict[str, object]:
    if duration_us <= 0:
        raise ValueError("duration must be positive")
    if sample_interval_us <= 0:
        raise ValueError("sample interval must be positive")

    root = _load_root(source)
    if root.findtext("version") != "1.1":
        raise ValueError("expected CVAT annotation version 1.1")
    width = int(root.findtext("meta/original_size/width", "0"))
    height = int(root.findtext("meta/original_size/height", "0"))
    frame_count = int(root.findtext("meta/job/size", "0"))
    if width <= 0 or height <= 0 or frame_count <= 0:
        raise ValueError("CVAT metadata has invalid dimensions or frame count")
    expected_duration_us = frame_rate.timestamp_us(frame_count)
    frame_duration_us = frame_rate.timestamp_us(1)
    if abs(duration_us - expected_duration_us) > frame_duration_us:
        raise ValueError(
            "CVAT frame count and supplied media duration differ by more than one frame"
        )

    grouped: dict[str, dict[str, object]] = {}
    source_track_count = 0
    for track in root.findall("track"):
        boxes = track.findall("box")
        if not boxes:
            continue
        source_track_count += 1
        attributes = _immutable_attributes(boxes)
        north_star_id = attributes.get("north_star_id", "").strip()
        kind = attributes.get("kind", "").strip()
        if not north_star_id:
            raise ValueError(f"CVAT track {track.attrib['id']} has no north_star_id")
        if kind not in TRACKED_KINDS:
            raise ValueError(
                f"CVAT track {track.attrib['id']} has unsupported kind {kind!r}"
            )
        reference = grouped.setdefault(north_star_id, {
            "id": north_star_id,
            "kind": kind,
            "entity_tracking": True,
            "intervals": [],
            "tracks": [],
            "checkpoints": [],
        })
        if reference["kind"] != kind:
            raise ValueError(
                f"north_star_id {north_star_id!r} uses multiple kinds"
            )
        for run_index, run in enumerate(_visible_runs(boxes)):
            source_track_id = track.attrib["id"]
            if run_index:
                source_track_id = f"{source_track_id}.{run_index + 1}"
            start_frame = int(run[0].attrib["frame"])
            end_frame = int(run[-1].attrib["frame"])
            start_us = frame_rate.timestamp_us(start_frame)
            end_us = min(duration_us, frame_rate.timestamp_us(end_frame + 1))
            reference["intervals"].append([start_us, end_us])
            reference["tracks"].append({
                "source_track_id": source_track_id,
                "start_frame": start_frame,
                "end_frame": end_frame,
                "start_us": start_us,
                "end_us": end_us,
                "visible_frame_count": len(run),
                "occluded_frame_count": sum(
                    box.attrib["occluded"] == "1" for box in run
                ),
            })
            reference["checkpoints"].extend(_sample_run(
                run,
                frame_rate,
                sample_interval_us,
                width,
                height,
                source_track_id,
            ))

    references = sorted(
        grouped.values(),
        key=lambda reference: (
            min(track["start_frame"] for track in reference["tracks"]),
            reference["id"],
        ),
    )
    for reference in references:
        ordering = lambda track: (track["start_frame"], track["source_track_id"])
        reference["tracks"].sort(key=ordering)
        reference["intervals"] = [
            [track["start_us"], track["end_us"]]
            for track in reference["tracks"]
        ]
        reference["checkpoints"].sort(
            key=lambda point: (point["pts_us"], point["source_track_id"])
        )

    return {
        video_name: {
            "duration_us": duration_us,
            "source": {
                "format": "CVAT for video 1.1",
                "frame_count": frame_count,
                "annotation_width": width,
                "annotation_height": height,
                "frame_rate_numerator": frame_rate.numerator,
                "frame_rate_denominator": frame_rate.denominator,
                "sample_interval_us": sample_interval_us,
                "source_track_count": source_track_count,
            },
            "references": references,
        }
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--video-name", required=True)
    parser.add_argument("--fps", required=True, help="rational rate, e.g. 24000/1001")
    parser.add_argument("--duration-us", required=True, type=int)
    parser.add_argument("--sample-interval-us", type=int, default=200_000)
    parser.add_argument("--output", required=True, type=Path)
    arguments = parser.parse_args()
    fixture = convert(
        arguments.source,
        arguments.video_name,
        FrameRate.parse(arguments.fps),
        arguments.duration_us,
        arguments.sample_interval_us,
    )
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps(fixture, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
