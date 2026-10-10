#!/usr/bin/env python3
"""Score a two-die M1-M20 OpenROAD guide as GRT or mock-DRT.

This is derived from the supplied ``eval.py`` evaluator, but it accepts
separate Bottom/Top DEF files, merges their non-overlapping track capacities,
can consume OpenROAD ``.segments`` files, and can deterministically generate a
synthetic thermal JSON when no real thermal solver output is available.

The score keeps the original 45/30/25 weights:
  * 45% wirelength
  * 30% GCell congestion
  * 25% TSV/hotspot spatial correlation

``--stage mock-drt`` is intentionally labelled a proxy.  LEF/DEF/guide files
do not contain detailed-route shapes or real DRC results.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import json
import math
import re
from collections import defaultdict
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable, Sequence
from checker import parse_guide
from checker import parse_layer_num
from checker import check
from checker import GuideRect
from checker import DieArea

import tse_framework_pybind_def as def_pybind
import tse_framework_pybind_lef as lef_pybind


WIRELENGTH_WEIGHT = 0.45
CONGESTION_WEIGHT = 0.30
THERMAL_WEIGHT = 0.25
OVERFLOW_LIMIT = 20.0
DEFAULT_TSV_COST_DBU = 2000.0
DEFAULT_VIA_COST_DBU = 0.0
TSV_LOWER_LAYER = "metal10"
TSV_UPPER_LAYER = "metal11"


@dataclass(frozen=True)
class TrackPattern:
    axis: str
    start: float
    count: int
    step: float
    layer: str

    def coordinates(self) -> list[float]:
        return [self.start + index * self.step for index in range(self.count)]


@dataclass(frozen=True)
class Component:
    die: str
    instance: str
    master: str
    x_dbu: float
    y_dbu: float
    status: str


@dataclass(frozen=True)
class DefData:
    path: Path
    die_name: str
    dbu_per_micron: float
    die: DieArea
    tracks: dict[str, dict[str, list[TrackPattern]]]
    gcell_statements: dict[str, list[tuple[float, int, float]]]
    components: tuple[Component, ...]


@dataclass(frozen=True)
class GCellGrid:
    x_lines: tuple[float, ...]
    y_lines: tuple[float, ...]
    die: DieArea
    dbu_per_micron: float

    @property
    def cols(self) -> int:
        return len(self.x_lines) - 1

    @property
    def rows(self) -> int:
        return len(self.y_lines) - 1


@dataclass(frozen=True)
class TSVPoint:
    net: str
    x_dbu: float
    y_dbu: float
    x_um_local: float
    y_um_local: float
    gcell_col: int
    gcell_row: int
    source: str
    overlap_area_dbu2: float


def canonical_layer(layer_name: str) -> str:
    number = parse_layer_num(layer_name)
    return f"Metal{number}" if number else layer_name.strip()


def die_of_layer(layer_name: str) -> str:
    number = parse_layer_num(layer_name)
    if number == 0:
        return "unknown"
    return "bottom" if number <= 10 else "top"


def is_cross_die(layer1: str, layer2: str) -> bool:
    return die_of_layer(layer1) != die_of_layer(layer2)


def empty_matrix(rows: int, cols: int, value: float = 0.0) -> list[list[float]]:
    return [[float(value) for _ in range(cols)] for _ in range(rows)]


def flatten(matrix: Sequence[Sequence[float]]) -> list[float]:
    return [float(value) for row in matrix for value in row]


def parse_lef_routing_layers(lef_definition : def_pybind.LefDefinition) -> set[str]:
    layers: set[str] = set()
    for lef_layer in lef_definition.GetLayers():
        if lef_layer.type == "ROUTING":
            layers.add(lef_layer.name)

    return layers


def parse_def(path: Path, die_name: str) -> DefData:
    content = path.read_text(encoding="utf-8", errors="replace")
    units = re.search(
        r"UNITS\s+DISTANCE\s+MICRONS\s+([\d.]+)\s*;",
        content,
        re.IGNORECASE,
    )
    if not units:
        raise ValueError(f"{path}: missing UNITS DISTANCE MICRONS")
    dbu = float(units.group(1))
    if dbu <= 0:
        raise ValueError(f"{path}: invalid DBU per micron {dbu}")

    die_match = re.search(
        r"DIEAREA\s*\(\s*(-?[\d.]+)\s+(-?[\d.]+)\s*\)\s*"
        r"\(\s*(-?[\d.]+)\s+(-?[\d.]+)\s*\)\s*;",
        content,
        re.IGNORECASE,
    )
    if not die_match:
        raise ValueError(f"{path}: missing rectangular DIEAREA")
    die = DieArea(*map(float, die_match.groups()))
    if die.width <= 0 or die.height <= 0:
        raise ValueError(f"{path}: invalid DIEAREA {die}")

    tracks: dict[str, dict[str, list[TrackPattern]]] = defaultdict(
        lambda: {"X": [], "Y": []}
    )
    track_re = re.compile(
        r"TRACKS\s+([XY])\s+(-?[\d.]+)\s+DO\s+(\d+)\s+"
        r"STEP\s+([\d.]+)\s+LAYER\s+(\w+)\s*;",
        re.IGNORECASE,
    )
    for match in track_re.finditer(content):
        axis = match.group(1).upper()
        start = float(match.group(2))
        count = int(match.group(3))
        step = float(match.group(4))
        layer = match.group(5)
        if count <= 0 or step <= 0:
            raise ValueError(f"{path}: invalid TRACKS statement {match.group(0)}")
        tracks[layer][axis].append(TrackPattern(axis, start, count, step, layer))
    if not tracks:
        raise ValueError(f"{path}: no TRACKS statements")

    gcell: dict[str, list[tuple[float, int, float]]] = {"X": [], "Y": []}
    gcell_re = re.compile(
        r"GCELLGRID\s+([XY])\s+(-?[\d.]+)\s+DO\s+(\d+)\s+"
        r"STEP\s+([\d.]+)\s*;",
        re.IGNORECASE,
    )
    for match in gcell_re.finditer(content):
        axis = match.group(1).upper()
        item = (float(match.group(2)), int(match.group(3)), float(match.group(4)))
        if item[1] < 2 or item[2] <= 0:
            raise ValueError(f"{path}: invalid GCELLGRID statement {match.group(0)}")
        gcell[axis].append(item)
    if not gcell["X"] or not gcell["Y"]:
        raise ValueError(f"{path}: both GCELLGRID X and Y are required")

    components: list[Component] = []
    section = re.search(
        r"\bCOMPONENTS\s+\d+\s*;(.*?)\bEND\s+COMPONENTS\b",
        content,
        re.IGNORECASE | re.DOTALL,
    )
    if section:
        comp_re = re.compile(
            r"-\s+(\S+)\s+(\S+).*?\+\s+"
            r"(FIXED|PLACED|COVER)\s+\(\s*(-?\d+)\s+(-?\d+)\s*\)",
            re.IGNORECASE | re.DOTALL,
        )
        for statement in section.group(1).split(";"):
            match = comp_re.search(statement)
            if match:
                components.append(
                    Component(
                        die=die_name,
                        instance=match.group(1),
                        master=match.group(2),
                        status=match.group(3).upper(),
                        x_dbu=float(match.group(4)),
                        y_dbu=float(match.group(5)),
                    )
                )

    return DefData(
        path=path,
        die_name=die_name,
        dbu_per_micron=dbu,
        die=die,
        tracks={layer: dict(axes) for layer, axes in tracks.items()},
        gcell_statements=gcell,
        components=tuple(components),
    )

def expand_grid_lines(
    statements: list[tuple[float, int, float]],
    die_lower: float,
    die_upper: float,
    axis: str,
) -> tuple[tuple[float, ...], list[str]]:
    values = [
        start + index * step
        for start, count, step in statements
        for index in range(count)
    ]
    lines = sorted(set(values))
    warnings: list[str] = []
    if len(lines) < 2:
        raise ValueError(f"GCELLGRID {axis} cannot form an interval")
    if lines[0] > die_lower:
        lines.insert(0, die_lower)
        warnings.append(f"GCELLGRID {axis} was extended to DIEAREA lower boundary")
    if lines[-1] < die_upper:
        lines.append(die_upper)
        warnings.append(
            f"GCELLGRID {axis} ended before DIEAREA; a clipped edge GCell was appended"
        )
    return tuple(lines), warnings


def build_gcell_grid(
    dbu: float,
    die: DieArea,
    statements: dict[str, list[tuple[float, int, float]]],
) -> tuple[GCellGrid, list[str]]:
    xs, x_warnings = expand_grid_lines(statements["X"], die.x1, die.x2, "X")
    ys, y_warnings = expand_grid_lines(statements["Y"], die.y1, die.y2, "Y")
    return GCellGrid(xs, ys, die, dbu), x_warnings + y_warnings


def locate_interval(lines: tuple[float, ...], coordinate: float) -> int | None:
    if coordinate < lines[0] or coordinate > lines[-1]:
        return None
    index = bisect.bisect_right(lines, coordinate) - 1
    if index == len(lines) - 1:
        index -= 1
    return index if 0 <= index < len(lines) - 1 else None


def rectangle_cell_indices(lower: float, upper: float, lines: tuple[float, ...]) -> range:
    if upper < lower:
        lower, upper = upper, lower
    if math.isclose(lower, upper):
        index = locate_interval(lines, lower)
        return range(0, 0) if index is None else range(index, index + 1)
    start = max(0, bisect.bisect_right(lines, lower) - 1)
    stop = min(len(lines) - 1, bisect.bisect_left(lines, upper))
    return range(start, stop)


def rect_center(rect: GuideRect) -> tuple[float, float]:
    return ((rect[0] + rect[2]) / 2.0, (rect[1] + rect[3]) / 2.0)


def edge_cost(a: GuideRect, b: GuideRect, tsv_cost: float, via_cost: float) -> float:
    ax, ay = rect_center(a)
    bx, by = rect_center(b)
    cost = abs(ax - bx) + abs(ay - by)
    if a[4] != b[4]:
        if is_cross_die(a[4], b[4]):
            cost += tsv_cost
        else:
            cost += via_cost * abs(parse_layer_num(a[4]) - parse_layer_num(b[4]))
    return cost


def guide_mst_wirelength(
    nets: dict[str, list[GuideRect]], tsv_cost: float, via_cost: float
) -> tuple[float, dict[str, float]]:
    total = 0.0
    per_net: dict[str, float] = {}
    for name, rectangles in nets.items():
        unique = list(dict.fromkeys(rectangles))
        if not unique:
            continue
        if len(unique) == 1:
            value = max(unique[0][2] - unique[0][0], unique[0][3] - unique[0][1])
        else:
            used = {0}
            value = 0.0
            while len(used) < len(unique):
                best: tuple[float, int] | None = None
                for left in used:
                    for right in range(len(unique)):
                        if right in used:
                            continue
                        candidate = (edge_cost(unique[left], unique[right], tsv_cost, via_cost), right)
                        if best is None or candidate < best:
                            best = candidate
                assert best is not None
                value += best[0]
                used.add(best[1])
        per_net[name] = value
        total += value
    return total, per_net


def guide_hpwl(rectangles: Sequence[GuideRect], tsv_cost: float) -> float:
    if not rectangles:
        return 0.0
    # Guide rectangles are routing corridors, not pin boxes.  Using their full
    # extents inflates the lower bound by one GCell on every net.  Centers give
    # a stable proxy for the terminal span when only guide/segment artifacts
    # are available.
    centers = [rect_center(rect) for rect in rectangles]
    xs = [point[0] for point in centers]
    ys = [point[1] for point in centers]
    dies = {die_of_layer(rect[4]) for rect in rectangles}
    return max(xs) - min(xs) + max(ys) - min(ys) + (tsv_cost if len(dies) > 1 else 0.0)


def track_count_per_interval(coordinates: Iterable[float], lines: tuple[float, ...]) -> list[int]:
    counts = [0] * (len(lines) - 1)
    for coordinate in coordinates:
        index = locate_interval(lines, coordinate)
        if index is not None:
            counts[index] += 1
    return counts


def build_capacity(
    tracks: dict[str, dict[str, list[TrackPattern]]], grid: GCellGrid
) -> dict[tuple[int, int, str], int]:
    capacity: dict[tuple[int, int, str], int] = {}
    for layer, axes in tracks.items():
        xs = [point for pattern in axes.get("X", []) for point in pattern.coordinates()]
        ys = [point for pattern in axes.get("Y", []) for point in pattern.coordinates()]
        x_counts = track_count_per_interval(xs, grid.x_lines)
        y_counts = track_count_per_interval(ys, grid.y_lines)
        for row in range(grid.rows):
            for col in range(grid.cols):
                capacity[(col, row, layer)] = x_counts[col] + y_counts[row]
    return capacity


def compute_congestion(
    nets: dict[str, list[GuideRect]],
    tracks: dict[str, dict[str, list[TrackPattern]]],
    grid: GCellGrid,
):
    demand: dict[tuple[int, int, str], int] = defaultdict(int)
    for rectangles in nets.values():
        for x1, y1, x2, y2, layer in rectangles:
            x1 = max(float(x1), grid.die.x1)
            y1 = max(float(y1), grid.die.y1)
            x2 = min(float(x2), grid.die.x2)
            y2 = min(float(y2), grid.die.y2)
            if x2 < x1 or y2 < y1:
                continue
            for col in rectangle_cell_indices(x1, x2, grid.x_lines):
                for row in rectangle_cell_indices(y1, y2, grid.y_lines):
                    demand[(col, row, layer)] += 1
    capacity = build_capacity(tracks, grid)
    overflow = []
    for key, amount in demand.items():
        cap = capacity.get(key, 0)
        if amount > cap:
            overflow.append((key, amount, cap, amount - cap))
    max_overflow = max((item[3] for item in overflow), default=0)
    total_overflow = sum(item[3] for item in overflow)
    average = total_overflow / len(demand) if demand else 0.0
    return max_overflow, average, total_overflow, overflow, dict(demand), capacity


def component_power(master: str) -> tuple[float, float]:
    name = master.upper()
    if "RESOURCE_SLOT" in name:
        return 0.0, 10.0
    if "TSV" in name:
        return 0.02, 6.0
    if "MACRO" in name or "RAM" in name:
        return 4.5, 13.0
    return 0.075, 7.0


def synthesize_thermal_json(
    output: Path,
    grid: GCellGrid,
    components: Sequence[Component],
) -> dict:
    by_die: dict[str, list[Component]] = defaultdict(list)
    for component in components:
        by_die[component.die].append(component)
    die_names = sorted(by_die) or ["die0"]
    ambient = 25.0
    nodes: list[list[float]] = []
    for z_index, die_name in enumerate(die_names):
        die_components = by_die.get(die_name, [])
        baseline = 6.0 + 0.8 * z_index
        for row in range(grid.rows):
            y_dbu = (grid.y_lines[row] + grid.y_lines[row + 1]) / 2.0
            y_um = (y_dbu - grid.die.y1) / grid.dbu_per_micron
            for col in range(grid.cols):
                x_dbu = (grid.x_lines[col] + grid.x_lines[col + 1]) / 2.0
                x_um = (x_dbu - grid.die.x1) / grid.dbu_per_micron
                temperature = ambient + baseline
                temperature += 0.35 * math.sin(math.pi * x_um / max(grid.die.width / grid.dbu_per_micron, 1.0))
                temperature += 0.25 * math.cos(math.pi * y_um / max(grid.die.height / grid.dbu_per_micron, 1.0))
                for component in die_components:
                    strength, sigma_um = component_power(component.master)
                    if strength == 0:
                        continue
                    cx = (component.x_dbu - grid.die.x1) / grid.dbu_per_micron
                    cy = (component.y_dbu - grid.die.y1) / grid.dbu_per_micron
                    distance2 = (x_um - cx) ** 2 + (y_um - cy) ** 2
                    temperature += strength * math.exp(-distance2 / (2.0 * sigma_um**2))
                nodes.append([round(x_um, 6), round(y_um, 6), float(z_index), round(temperature, 6)])
    payload = {
        "meta": {
            "synthetic": True,
            "signoff_usable": False,
            "generator": "eval_mock_route.py deterministic placement-density proxy",
            "ambientTemp": ambient,
            "gridResolution": [grid.cols, grid.rows],
            "nodeLayout": "one node at every DEF GCell center for each die",
            "dieNames": die_names,
            "temperatureUnit": "degC",
            "coordinateUnit": "micron",
            "warning": "Synthetic heat map for scorer bring-up only; replace with solver output for research results.",
        },
        "nodes": nodes,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    return payload


def load_thermal_json(path: Path) -> tuple[dict[tuple[float, float], float], dict]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    nodes = payload.get("nodes")
    if not isinstance(nodes, list) or not nodes:
        raise ValueError(f"{path}: nodes must be a non-empty list")
    grouped: dict[tuple[float, float], list[float]] = defaultdict(list)
    for index, node in enumerate(nodes):
        if not isinstance(node, list) or len(node) < 4:
            raise ValueError(f"{path}: invalid nodes[{index}]")
        x, y, _z, temperature = map(float, node[:4])
        if not all(math.isfinite(value) for value in (x, y, temperature)):
            raise ValueError(f"{path}: NaN/Inf in nodes[{index}]")
        grouped[(round(x, 9), round(y, 9))].append(temperature)
    projected = {point: sum(values) / len(values) for point, values in grouped.items()}
    return projected, payload


def aggregate_temperature(
    projected: dict[tuple[float, float], float], grid: GCellGrid
) -> tuple[list[list[float]], list[list[float]], int]:
    sums = empty_matrix(grid.rows, grid.cols)
    counts = empty_matrix(grid.rows, grid.cols)
    mapped = 0
    for (x_um, y_um), temperature in projected.items():
        x_dbu = grid.die.x1 + x_um * grid.dbu_per_micron
        y_dbu = grid.die.y1 + y_um * grid.dbu_per_micron
        col = locate_interval(grid.x_lines, x_dbu)
        row = locate_interval(grid.y_lines, y_dbu)
        if col is not None and row is not None:
            sums[row][col] += temperature
            counts[row][col] += 1
            mapped += 1
    heat = empty_matrix(grid.rows, grid.cols)
    missing = []
    for row in range(grid.rows):
        for col in range(grid.cols):
            if counts[row][col] == 0:
                missing.append((col, row))
            else:
                heat[row][col] = sums[row][col] / counts[row][col]
    if missing:
        preview = ", ".join(f"({col},{row})" for col, row in missing[:12])
        raise ValueError(f"thermal JSON leaves {len(missing)} GCells empty; first: {preview}")
    return heat, counts, mapped

def extract_tsv_from_def(def_definition : def_pybind.DefDefinition, grid: GCellGrid) -> list[TSVPoint]:
    tsv_list = []
    nets = def_definition.getNets()
    for component in def_definition.getComponents():
        if component.macro_name == "TSV_BOTTOM_M10_5X5_KOZ1" or component.macro_name == "TSV_TOP_M11_5X5_KOZ1":
            net_name = next((net.name for net in nets for connection in net.connections if connection.instance == component.instance_name), "redundancy")
            x_dbu = component.x
            y_dbu = component.y
            x_um_local=(x_dbu - grid.die.x1) / grid.dbu_per_micron,
            y_um_local=(y_dbu - grid.die.y1) / grid.dbu_per_micron,
            gcell_col = locate_interval(grid.x_lines, x_dbu)
            gcell_row = locate_interval(grid.y_lines, y_dbu)

            if tsv_list and next((True for tsv in tsv_list if tsv.gcell_col == gcell_col and tsv.gcell_row == gcell_row), False):
                continue

            tsv = TSVPoint(net_name, x_dbu, y_dbu, x_um_local, y_um_local, gcell_col, gcell_row, "M10_M11_guide_overlap", 0.0)
            tsv_list.append(tsv)

    return tsv_list

def extract_tsv_points(nets: dict[str, list[GuideRect]], grid: GCellGrid) -> list[TSVPoint]:
    points: list[TSVPoint] = []
    for net, rectangles in nets.items():
        lower = [rect for rect in rectangles if rect[4] == TSV_LOWER_LAYER]
        upper = [rect for rect in rectangles if rect[4] == TSV_UPPER_LAYER]
        candidates = []
        for a in lower:
            for b in upper:
                x1, y1 = max(a[0], b[0]), max(a[1], b[1])
                x2, y2 = min(a[2], b[2]), min(a[3], b[3])
                if x1 <= x2 and y1 <= y2:
                    candidates.append(((x2 - x1) * (y2 - y1), x1, y1, x2, y2))
        if not candidates:
            continue
        area, x1, y1, x2, y2 = max(candidates)
        x_dbu, y_dbu = (x1 + x2) / 2.0, (y1 + y2) / 2.0
        col = locate_interval(grid.x_lines, x_dbu)
        row = locate_interval(grid.y_lines, y_dbu)
        if col is not None and row is not None:
            points.append(
                TSVPoint(
                    net=net,
                    x_dbu=x_dbu,
                    y_dbu=y_dbu,
                    x_um_local=(x_dbu - grid.die.x1) / grid.dbu_per_micron,
                    y_um_local=(y_dbu - grid.die.y1) / grid.dbu_per_micron,
                    gcell_col=col,
                    gcell_row=row,
                    source="M10_M11_guide_overlap",
                    overlap_area_dbu2=float(area),
                )
            )
    return points


def tsv_matrix(points: Sequence[TSVPoint], grid: GCellGrid) -> list[list[float]]:
    matrix = empty_matrix(grid.rows, grid.cols)
    for point in points:
        matrix[point.gcell_row][point.gcell_col] += 1.0
    return matrix


def pearson_heat_tsv(
    heat: Sequence[Sequence[float]],
    tsvs: Sequence[Sequence[float]],
    ambient: float,
) -> float:
    heat_values = [max(value - ambient, 0.0) for value in flatten(heat)]
    tsv_values = flatten(tsvs)
    if sum(tsv_values) <= 0:
        raise ValueError("no TSVs were available for thermal correlation")
    if sum(heat_values) <= 0:
        raise ValueError("thermal map contains no temperature rise above ambient")
    heat_values = [value / sum(heat_values) for value in heat_values]
    tsv_values = [value / sum(tsv_values) for value in tsv_values]
    heat_mean = sum(heat_values) / len(heat_values)
    tsv_mean = sum(tsv_values) / len(tsv_values)
    numerator = sum(
        (h - heat_mean) * (t - tsv_mean) for h, t in zip(heat_values, tsv_values)
    )
    denominator = math.sqrt(sum((h - heat_mean) ** 2 for h in heat_values)) * math.sqrt(
        sum((t - tsv_mean) ** 2 for t in tsv_values)
    )
    if math.isclose(denominator, 0.0, abs_tol=1e-15):
        return 0.0
    return max(-1.0, min(1.0, numerator / denominator))


def compute_score(norm_l: float, max_overflow: float, correlation: float) -> dict:
    q_wl = min(1.0, 1.0 / max(norm_l, 1.0))
    q_cong = max(0.0, 1.0 - max_overflow / OVERFLOW_LIMIT)
    q_thermal = (correlation + 1.0) / 2.0
    wl_score = 100.0 * WIRELENGTH_WEIGHT * q_wl
    congestion_score = 100.0 * CONGESTION_WEIGHT * q_cong
    thermal_score = 100.0 * THERMAL_WEIGHT * q_thermal
    total = wl_score + congestion_score + thermal_score
    if total >= 90:
        grade = "A+"
    elif total >= 85:
        grade = "A"
    elif total >= 80:
        grade = "A-"
    elif total >= 75:
        grade = "B+"
    elif total >= 70:
        grade = "B"
    elif total >= 65:
        grade = "B-"
    elif total >= 60:
        grade = "C+"
    elif total >= 55:
        grade = "C"
    elif total >= 50:
        grade = "C-"
    elif total >= 40:
        grade = "D"
    else:
        grade = "F"
    return {
        "total_score": total,
        "grade": grade,
        "wirelength_score": wl_score,
        "congestion_score": congestion_score,
        "thermal_score": thermal_score,
        "wirelength_quality": q_wl,
        "congestion_quality": q_cong,
        "thermal_quality": q_thermal,
        "weights": {"wirelength": 0.45, "congestion": 0.30, "thermal_tsv": 0.25},
    }


def write_matrix(path: Path, matrix: Sequence[Sequence[float]], fmt: str) -> None:
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        for row in matrix:
            writer.writerow([format(float(value), fmt) for value in row])


def write_tsv_points(path: Path, points: Sequence[TSVPoint]) -> None:
    if not points:
        return
    with path.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(asdict(points[0])))
        writer.writeheader()
        for point in points:
            writer.writerow(asdict(point))


def write_overflow(path: Path, overflow) -> None:
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["gcell_col", "gcell_row", "layer", "demand", "capacity", "overflow"])
        for (col, row, layer), demand, capacity, amount in sorted(
            overflow, key=lambda item: (-item[3], parse_layer_num(item[0][2]), item[0][1], item[0][0])
        ):
            writer.writerow([col, row, layer, demand, capacity, amount])


def _create_parser() ->  argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Score M1-M20 GRT guides or a guide-derived mock-DRT result."
    )
    parser.add_argument("guide_file", type=Path)
    parser.add_argument("--lefs", action="append", type=Path, required=True)
    parser.add_argument("--input-def", type=Path, required=True)
    parser.add_argument("--thermal-json", type=Path, required=True)
    parser.add_argument("--tsv-cost-dbu", type=float, default=DEFAULT_TSV_COST_DBU)
    parser.add_argument("--via-cost-dbu", type=float, default=DEFAULT_VIA_COST_DBU)
    parser.add_argument("--output-dir", type=Path, required=True)

    return parser

def _evaluate(args, parser : argparse.ArgumentParser) -> str:
    args.output_dir.mkdir(parents=True, exist_ok=True)
    try:
        def_definition = def_pybind.DefParser().Parse(args.input_def.as_posix())
        lef_definition = lef_pybind.LefParser().Parse([lef_path.as_posix() for lef_path in args.lefs])

        warnings: list[str] = []

        def_data = parse_def(args.input_def, "def")
        dbu = def_data.dbu_per_micron
        die = def_data.die
        tracks = def_data.tracks
        gcell_statements = def_data.gcell_statements

        grid, grid_warnings = build_gcell_grid(dbu, die, gcell_statements)
        warnings.extend(grid_warnings)

        lef_layers = parse_lef_routing_layers(lef_definition)
        nets = parse_guide(args.guide_file)

        check(nets, def_definition, lef_definition)

        guide_layers = {rect[4] for rectangles in nets.values() for rect in rectangles}
        missing_lef = sorted(guide_layers - lef_layers, key=parse_layer_num)
        if missing_lef:
            warnings.append("guide layers missing from LEF: " + ", ".join(missing_lef))

        missing_tracks = sorted(guide_layers - set(tracks), key=parse_layer_num)
        if missing_tracks:
            warnings.append("guide layers missing from merged DEF TRACKS: " + ", ".join(missing_tracks))

        tsv_points = extract_tsv_points(nets, grid)
        if not tsv_points:
            raise ValueError("no TSV points found")

        guide_proxy_wl, per_net_proxy = guide_mst_wirelength(
            nets, args.tsv_cost_dbu, args.via_cost_dbu
        )
        hpwl = sum(guide_hpwl(rectangles, args.tsv_cost_dbu) for rectangles in nets.values())
        total_wl = guide_proxy_wl
        route_fidelity = "guide-rectangle Manhattan MST proxy"
        norm_l = total_wl / hpwl if hpwl > 0 else 1.0

        max_overflow, avg_overflow, total_overflow, overflow, demand, capacity = compute_congestion(
            nets, tracks, grid
        )

        thermal_path = args.thermal_json
        projected, thermal_payload = load_thermal_json(thermal_path)

        heat, node_counts, mapped_nodes = aggregate_temperature(projected, grid)
        ambient = float(thermal_payload.get("meta", {}).get("ambientTemp", 25.0))
        tsvs = tsv_matrix(tsv_points, grid)
        correlation = pearson_heat_tsv(heat, tsvs, ambient)
        score = compute_score(norm_l, max_overflow, correlation)

        write_matrix(args.output_dir / "heat_gcell.csv", heat, ".8f")
        write_matrix(args.output_dir / "tsv_gcell.csv", tsvs, ".0f")
        write_matrix(args.output_dir / "thermal_node_count_per_gcell.csv", node_counts, ".0f")
        write_tsv_points(args.output_dir / "tsv_points.csv", tsv_points)
        write_overflow(args.output_dir / "overflow_gcells.csv", overflow)

        synthetic = bool(thermal_payload.get("meta", {}).get("synthetic", False))
        synthetic_warning = "thermal score uses a deterministic synthetic placement-density map"
        if synthetic and synthetic_warning not in warnings:
            warnings.append(synthetic_warning)

        report_data = {
            "status": "PASS",
            "fidelity": {
                "wirelength": route_fidelity,
                "congestion": "GRT guide demand / merged DEF track capacity",
                "drc": "not available; overflow entries are only a mock-DRT risk proxy",
                "thermal": "synthetic" if synthetic else "external",
            },
            "inputs": {
                "guide": str(args.guide_file),
                "lefs": [str(lef_file) for lef_file in args.lefs],
                "defs": [args.input_def.as_posix()],
                "thermal_json": str(thermal_path),
            },
            "grid": {
                "dbu_per_micron": dbu,
                "rows": grid.rows,
                "cols": grid.cols,
                "mapped_thermal_xy_nodes": mapped_nodes,
            },
            "wirelength": {
                "total_dbu": total_wl,
                "total_um": total_wl / dbu,
                "guide_mst_proxy_dbu": guide_proxy_wl,
                "hpwl_reference_dbu": hpwl,
                "normalized": norm_l,
                "tsv_count": len(tsv_points),
                "tsv_cost_dbu_each": args.tsv_cost_dbu,
            },
            "congestion": {
                "max_overflow": max_overflow,
                "average_overflow": avg_overflow,
                "total_overflow": total_overflow,
                "used_layer_gcells": len(demand),
                "overflow_entries": len(overflow),
                "mock_drt_risk_hotspots": len(overflow),
            },
            "thermal_tsv": {
                "synthetic": synthetic,
                "ambient_temperature": ambient,
                "tsv_points": len(tsv_points),
                "correlation_above_ambient": correlation,
            },
            "score": score,
            "warnings": warnings,
        }
        json_path = args.output_dir / "evaluation_report.json"
        text_to_output = json.dumps(report_data, indent=2)
        json_path.write_text(text_to_output + "\n", encoding="utf-8")

        lines = [
            "=" * 78,
            "Global Routing Evaluation Report",
            "=" * 78,
            f"Wirelength fidelity:           {route_fidelity}",
            f"Thermal source:                {'SYNTHETIC (not signoff)' if synthetic else 'external JSON'}",
            f"GCell grid:                    {grid.rows} rows x {grid.cols} cols",
            f"Guide nets / rectangles:       {len(nets)} / {sum(map(len, nets.values()))}",
            f"TSV points:                    {len(tsv_points)}",
            "",
            "--- Wirelength ---",
            f"Total:                         {total_wl / dbu:.4f} um",
            f"HPWL reference:                {hpwl / dbu:.4f} um",
            f"Normalized length:             {norm_l:.8f}",
            "",
            "--- Guide Congestion / Mock-DRT Risk ---",
            f"Max overflow:                  {max_overflow}",
            f"Average overflow:              {avg_overflow:.8f}",
            f"Total overflow:                {total_overflow}",
            f"Overflow GCell-layer entries:  {len(overflow)}",
            "Real detailed-route DRC:       NOT AVAILABLE FROM GUIDE",
            "",
            "--- Thermal / TSV ---",
            f"Correlation above ambient:     {correlation:.10f}",
            "",
            "--- Score (original 45/30/25 weights) ---",
            f"Wirelength:                    {score['wirelength_score']:.4f} / 45",
            f"Congestion:                    {score['congestion_score']:.4f} / 30",
            f"Thermal/TSV:                   {score['thermal_score']:.4f} / 25",
            f"Total:                         {score['total_score']:.4f} / 100",
            f"Grade:                         {score['grade']}",
            "",
            "IMPORTANT: mock-drt is a guide/segment proxy, not a replacement for TritonRoute DRC.",
        ]
        if warnings:
            lines.extend(["", "--- Warnings ---", *[f"- {warning}" for warning in warnings]])
        text = "\n".join(lines) + "\n"
        (args.output_dir / "evaluation_report.txt").write_text(text, encoding="utf-8")
        print(text, end="")
        print(f"JSON report: {json_path}")
        print(f"Thermal JSON: {thermal_path}")

        return json_path
    except (OSError, ValueError, json.JSONDecodeError) as error:
        parser.error(str(error))
        return ""


def run(argvs : list) -> None:
    parser = _create_parser()
    args = parser.parse_args(argvs)
    return _evaluate(args, parser)

def main() -> None:
    parser = _create_parser()
    args = parser.parse_args()
    _evaluate(args, parser)

if __name__ == "__main__":
    main()
