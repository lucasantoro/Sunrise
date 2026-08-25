#!/usr/bin/env python3
"""Extract and inspect one STM32 RXFAIL edge-interval trace."""

from __future__ import annotations

import argparse
import csv
import json
import re
from dataclasses import dataclass
from pathlib import Path


BEGIN_RE = re.compile(r"RXFAIL_BEGIN\s+(?P<fields>.*)")
DATA_RE = re.compile(
    r"RXFAIL_DATA\s+id=(?P<id>\d+)\s+off=(?P<off>\d+)\s+hex=(?P<hex>[0-9a-fA-F]+)"
)
END_RE = re.compile(
    r"RXFAIL_END\s+id=(?P<id>\d+)\s+runs=(?P<runs>\d+)\s+clipped=(?P<clipped>\d+)"
)
FIELD_RE = re.compile(r"(\w+)=(-?\d+)")


@dataclass
class FailureTrace:
    metadata: dict[str, int]
    intervals: list[int]


def parse_trace(text: str) -> FailureTrace:
    metadata: dict[str, int] | None = None
    intervals: list[int] = []
    ended = False
    first_data_offset: int | None = None

    for line in text.splitlines():
        begin = BEGIN_RE.search(line)
        if begin:
            metadata = {
                key: int(value)
                for key, value in FIELD_RE.findall(begin.group("fields"))
            }
            if metadata.get("v") != 1 or metadata.get("runs", 0) <= 0:
                raise ValueError("invalid RXFAIL_BEGIN")
            intervals = []
            ended = False
            continue

        data = DATA_RE.search(line)
        if data and first_data_offset is None:
            first_data_offset = int(data.group("off"))
        if data and metadata is not None:
            trace_id = int(data.group("id"))
            offset = int(data.group("off"))
            encoded = data.group("hex")
            if trace_id != metadata["id"]:
                raise ValueError("RXFAIL_DATA id does not match BEGIN")
            if offset != len(intervals):
                raise ValueError(
                    f"non-contiguous RXFAIL_DATA: off={offset}, "
                    f"expected={len(intervals)}"
                )
            if len(encoded) % 4:
                raise ValueError("RXFAIL_DATA hex length is not a multiple of 4")
            intervals.extend(
                int(encoded[index : index + 4], 16)
                for index in range(0, len(encoded), 4)
            )
            continue

        end = END_RE.search(line)
        if end and metadata is not None:
            if int(end.group("id")) != metadata["id"]:
                raise ValueError("RXFAIL_END id does not match BEGIN")
            if int(end.group("runs")) != metadata["runs"]:
                raise ValueError("RXFAIL_END run count does not match BEGIN")
            if int(end.group("clipped")):
                raise ValueError("trace contains clipped intervals")
            ended = True

    if metadata is None and first_data_offset is not None:
        raise ValueError(
            "incomplete RXFAIL trace: RXFAIL_BEGIN is missing and the first "
            f"available RXFAIL_DATA offset is {first_data_offset}; export the "
            "journal from RXFAIL_BEGIN through RXFAIL_END"
        )
    if metadata is None:
        raise ValueError("RXFAIL_BEGIN not found")
    if not ended:
        raise ValueError("RXFAIL_END not found")
    if len(intervals) != metadata["runs"]:
        raise ValueError(
            f"incomplete trace: received {len(intervals)}/"
            f"{metadata['runs']} intervals"
        )
    return FailureTrace(metadata, intervals)


def quantize_interval(
    run: int, parity: int, t0: int, t1: int, nominal: int, bias_div: int = 16
) -> tuple[int, int, int, float]:
    """Mirror comp_quantize_run() and report distance from its nearest boundary."""
    base = (t0, t1)[parity]
    measured_q8 = run << 8
    nominal_q8 = nominal << 8
    base_q8 = base << 8
    decision_q8 = max(measured_q8 - nominal_q8 // bias_div, 0)
    cells = 1
    if decision_q8 > base_q8:
        rounded = decision_q8 - base_q8 + nominal_q8 // 2
        if rounded:
            rounded -= 1
        cells += rounded // nominal_q8
    cells = min(cells, 8)
    expected = base + (cells - 1) * nominal
    residual = run - expected

    delta_q8 = max(decision_q8 - base_q8, 0)
    lower_boundary_index = max(cells - 1, 0)
    boundaries = []
    if lower_boundary_index:
        boundaries.append((2 * lower_boundary_index - 1) * nominal_q8 // 2)
    boundaries.append((2 * lower_boundary_index + 1) * nominal_q8 // 2)
    margin_ticks = min(abs(delta_q8 - boundary) for boundary in boundaries) / 256
    return cells, expected, residual, margin_ticks


def analyze_trace(trace: FailureTrace) -> tuple[dict[str, object], list[dict[str, object]]]:
    metadata = trace.metadata
    t0 = metadata["t0"]
    t1 = metadata["t1"]
    nominal = metadata["tn"]
    timestamp = 0
    rows: list[dict[str, object]] = []

    for index, run in enumerate(trace.intervals):
        parity = index & 1
        cells, expected, residual, margin = quantize_interval(
            run, parity, t0, t1, nominal
        )
        timestamp += run
        rows.append(
            {
                "interval_index": index,
                "edge_index": index + 1,
                "timestamp_ticks": timestamp,
                "interval_ticks": run,
                "parity": parity,
                "quantized_cells": cells,
                "expected_ticks": expected,
                "residual_ticks": residual,
                "decision_margin_ticks": round(margin, 4),
            }
        )

    ambiguous = [row for row in rows if row["decision_margin_ticks"] <= 2.0]
    cell_histogram: dict[str, int] = {}
    for row in rows:
        key = str(row["quantized_cells"])
        cell_histogram[key] = cell_histogram.get(key, 0) + 1
    summary: dict[str, object] = {
        **metadata,
        "duration_ticks": timestamp,
        "duration_us": timestamp * 1_000_000 / metadata["tick_hz"],
        "cell_histogram": cell_histogram,
        "ambiguous_intervals_2ticks": len(ambiguous),
        "closest_decisions": sorted(
            (
                {
                    "interval_index": row["interval_index"],
                    "interval_ticks": row["interval_ticks"],
                    "parity": row["parity"],
                    "quantized_cells": row["quantized_cells"],
                    "margin_ticks": row["decision_margin_ticks"],
                }
                for row in rows
            ),
            key=lambda item: item["margin_ticks"],
        )[:32],
    }
    return summary, rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("journal", type=Path)
    parser.add_argument(
        "--output-prefix",
        type=Path,
        help="Output path without extension (default: journal directory/rxfail-ID)",
    )
    args = parser.parse_args()

    trace = parse_trace(args.journal.read_text(encoding="utf-8", errors="replace"))
    summary, rows = analyze_trace(trace)
    prefix = args.output_prefix or args.journal.with_name(
        f"rxfail-{trace.metadata['id']}"
    )
    summary_path = prefix.with_suffix(".json")
    csv_path = prefix.with_suffix(".csv")
    summary_path.write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    with csv_path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    print(json.dumps(summary, indent=2, sort_keys=True))
    print(f"wrote {summary_path}")
    print(f"wrote {csv_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
