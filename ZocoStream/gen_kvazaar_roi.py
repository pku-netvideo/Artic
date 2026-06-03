#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import json
import math
import argparse
from typing import List, Optional, Tuple

BBox = Optional[Tuple[float, float, float, float]]  # (xmin, ymin, xmax, ymax)


def load_bbox_from_json(json_path: str) -> BBox:
    """Load one pred_*.json file and return the last bbox in result."""
    with open(json_path, "r", encoding="utf-8") as f:
        data = json.load(f)

    if not data.get("ok", False):
        return None

    result = data.get("result", None)
    if not result or not isinstance(result, list):
        return None

    last = result[-1]
    # last: [ [xmin, ymin], [xmax, ymax] ]
    try:
        (xmin, ymin), (xmax, ymax) = last
        xmin, ymin, xmax, ymax = float(xmin), float(ymin), float(xmax), float(ymax)
    except Exception:
        return None

    eps = 1e-6
    if abs(xmax - xmin) < eps and abs(ymax - ymin) < eps:
        # Treat a zero-size box at the origin as missing ROI.
        if abs(xmin) < eps and abs(ymin) < eps:
            return None
    
    # Normalize potentially reversed coordinates.
    xmin, xmax = (xmin, xmax) if xmin <= xmax else (xmax, xmin)
    ymin, ymax = (ymin, ymax) if ymin <= ymax else (ymax, ymin)
    return (xmin, ymin, xmax, ymax)


def load_2fps_bboxes(roi_dir: str, num_frames_2fps: int = 10) -> List[BBox]:
    """Load pred_00001.json through pred_00010.json by default."""
    bboxes: List[BBox] = []
    for i in range(1, num_frames_2fps + 1):
        name = f"pred_{i:05d}.json"
        path = os.path.join(roi_dir, name)
        if not os.path.exists(path):
            raise FileNotFoundError(f"ROI json not found: {path}")
        bboxes.append(load_bbox_from_json(path))
    return bboxes


def parse_start_time_1dp(raw: str) -> str:
    """Format start_time the same way ROI prediction folders are named."""
    try:
        return f"{float(raw):.1f}"
    except Exception:
        return str(raw).strip()


def build_roi_dir(roi_root: str, sample_id: str, start_time: str, original_index: str) -> str:
    start_time_1dp = parse_start_time_1dp(start_time)
    return os.path.join(
        roi_root,
        f"sample_{sample_id}_start_{start_time_1dp}s",
        f"q_{original_index}",
    )


def format_fps_tag(fps: float) -> str:
    if float(fps).is_integer():
        return f"{int(fps)}fps"
    return f"{fps:g}fps"


def build_output_path(
    baseline_root: str,
    sample_id: str,
    original_index: str,
    fps_out: float,
) -> str:
    fps_tag = format_fps_tag(fps_out)
    return os.path.join(
        baseline_root,
        f"sample_{sample_id}",
        f"roi_{fps_tag}_q_{original_index}.txt",
    )


def lerp(a: float, b: float, t: float) -> float:
    return a * (1.0 - t) + b * t


def interp_bbox(b0: BBox, b1: BBox, t: float) -> BBox:
    """Linearly interpolate between two bounding boxes.

    If either endpoint is None, reuse the non-None endpoint. If both are None,
    return None.
    """
    if b0 is None and b1 is None:
        return None
    if b0 is None:
        return b1
    if b1 is None:
        return b0
    x0, y0, x1, y1 = b0
    x2, y2, x3, y3 = b1
    return (
        lerp(x0, x2, t),
        lerp(y0, y2, t),
        lerp(x1, x3, t),
        lerp(y1, y3, t),
    )


def bboxes_2fps_to_15fps(
    bboxes_2fps: List[BBox],
    fps_in: float = 2.0,
    fps_out: float = 15.0,
    duration_s: float = 5.0,
) -> List[BBox]:
    """Interpolate 2 FPS ROI boxes to the requested output FPS."""
    n_out = int(round(duration_s * fps_out))  # 75
    n_in = len(bboxes_2fps)                  # 10
    dt_in = 1.0 / fps_in                     # 0.5
    dt_out = 1.0 / fps_out

    out: List[BBox] = []
    for j in range(n_out):
        t = j * dt_out
        # Locate the surrounding input-frame interval.
        i = int(math.floor(t / dt_in))
        if i <= 0:
            out.append(bboxes_2fps[0])
            continue
        if i >= n_in - 1:
            out.append(bboxes_2fps[-1])
            continue
        t0 = i * dt_in
        alpha = (t - t0) / dt_in  # in [0,1)
        out.append(interp_bbox(bboxes_2fps[i], bboxes_2fps[i + 1], alpha))
    return out


def clamp(v: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, v))


def ctu_center(col: int, row: int, ctu: int, width: int, height: int) -> Tuple[float, float]:
    """Return the CTU center point in pixel coordinates."""
    cx = col * ctu + ctu / 2.0
    cy = row * ctu + ctu / 2.0
    # Clamp centers near the right/bottom image edge.
    cx = min(cx, width - 1.0)
    cy = min(cy, height - 1.0)
    return cx, cy


def point_rect_distance(px: float, py: float, xmin: float, ymin: float, xmax: float, ymax: float) -> float:
    """Return Euclidean distance from a point to an axis-aligned rectangle."""
    dx = 0.0
    if px < xmin:
        dx = xmin - px
    elif px > xmax:
        dx = px - xmax

    dy = 0.0
    if py < ymin:
        dy = ymin - py
    elif py > ymax:
        dy = py - ymax

    return math.sqrt(dx * dx + dy * dy)


def bbox_to_qp_delta_map(
    bbox: BBox,
    width: int,
    height: int,
    ctu: int = 64,
    gamma: float = 2.0,
    base_qp: int = 26,
) -> Tuple[int, int, List[int]]:
    """
    Convert a normalized ROI bbox into a Kvazaar CTU delta-QP map.

    CTUs inside the box keep the strongest response. CTUs outside the box use
    a distance-based falloff, then the response is converted to delta QP.
    - QP_abs = 51 * (1 - response^gamma)
    - delta = QP_abs - base_qp, clamped for Kvazaar.
    Returns (map_w, map_h, values[raster]).
    """
    map_w = int(math.ceil(width / ctu))
    map_h = int(math.ceil(height / ctu))

    # No ROI: assign a uniform background delta QP map.
    if bbox is None:
        map_w = int(math.ceil(width / ctu))
        map_h = int(math.ceil(height / ctu))
        vals = [18] * (map_w * map_h)
        return map_w, map_h, vals

    xmin, ymin, xmax, ymax = bbox
    xmin = xmin / 1000.0 * width
    xmax = xmax / 1000.0 * width
    ymin = ymin / 1000.0 * height
    ymax = ymax / 1000.0 * height
    # Clamp ROI coordinates to image bounds.
    xmin = clamp(xmin, 0, width - 1)
    xmax = clamp(xmax, 0, width - 1)
    ymin = clamp(ymin, 0, height - 1)
    ymax = clamp(ymax, 0, height - 1)

    # Use the farthest CTU distance to normalize linear falloff.
    dists = []
    inside = [False] * (map_w * map_h)

    for r in range(map_h):
        for c in range(map_w):
            idx = r * map_w + c
            cx, cy = ctu_center(c, r, ctu, width, height)
            d = point_rect_distance(cx, cy, xmin, ymin, xmax, ymax)
            if d == 0.0:
                inside[idx] = True
            else:
                dists.append(d)

    d_max = max(dists) if dists else 1.0  # Avoid division by zero.

    vals: List[int] = []
    for r in range(map_h):
        for c in range(map_w):
            idx = r * map_w + c
            if inside[idx]:
                resp = 1.0
            else:
                cx, cy = ctu_center(c, r, ctu, width, height)
                d = point_rect_distance(cx, cy, xmin, ymin, xmax, ymax)
                # Linear falloff: d=0 -> 1, d=d_max -> 0.
                resp = 1.0 - (d / d_max)
                resp = clamp(resp, 0.0, 1.0)

            qp_abs = 51.0 * (1.0 - (resp ** gamma))
            qp_abs = clamp(qp_abs, 0.0, 51.0)
            qp_abs_i = int(round(qp_abs))

            delta = int(round(qp_abs_i - base_qp))
            delta = int(clamp(delta, 0, 25))
            vals.append(delta)

    return map_w, map_h, vals


def write_kvazaar_roi_txt(
    out_path: str,
    maps: List[Tuple[int, int, List[int]]],
) -> None:
    """
    Write multi-frame Kvazaar ROI maps in text format.

    Each frame starts with map width and height, followed by raster-order
    delta-QP values.
    """
    with open(out_path, "w", encoding="utf-8") as f:
        for (mw, mh, vals) in maps:
            if len(vals) != mw * mh:
                raise ValueError("Map size mismatch.")
            f.write(f"{mw} {mh}\n")
            # Write one row of CTU values per line.
            for r in range(mh):
                row = vals[r * mw:(r + 1) * mw]
                f.write(" ".join(str(v) for v in row) + "\n")
            # Blank line between frames for readability.
            f.write("\n")


def main():
    ap = argparse.ArgumentParser(
        description="Generate kvazaar roi.txt (CTU delta QP map) from 2fps ROI json with 15fps interpolation."
    )
    ap.add_argument("--roi_root", required=True, help="Root folder containing ROI prediction folders.")
    ap.add_argument("--baseline_root", required=True, help="Root folder where sample outputs are written.")
    ap.add_argument("--sample_id", required=True, help="Sample id used in sample_<sample_id> folders.")
    ap.add_argument("--start_time", required=True, help="Sample start time; formatted to one decimal for ROI lookup.")
    ap.add_argument("--original_index", required=True, help="Question id used in q_<original_index> folders.")
    ap.add_argument("--width", type=int, required=True, help="Target encode width, e.g., 320 or 640")
    ap.add_argument("--height", type=int, required=True, help="Target encode height, e.g., 240 or 360")
    ap.add_argument("--gamma", type=float, default=2.0, choices=[2.0, 3.0, 4.0, 5.0, 6.0], help="Gamma in QP formula (2 or 3)")
    ap.add_argument("--ctu", type=int, default=64, help="CTU size (kvazaar default is 64)")
    ap.add_argument("--base_qp", type=int, default=26, help="Base QP used to convert abs QP to delta QP (suggest 26)")
    ap.add_argument("--fps_in", type=float, default=2.0)
    ap.add_argument("--fps_out", type=float, default=30.0)
    ap.add_argument("--duration", type=float, default=5.0)
    ap.add_argument("--num_in", type=int, default=10, help="Number of 2fps frames (default 10)")

    args = ap.parse_args()

    roi_dir = build_roi_dir(args.roi_root, args.sample_id, args.start_time, args.original_index)
    out_path = build_output_path(args.baseline_root, args.sample_id, args.original_index, args.fps_out)

    b2 = load_2fps_bboxes(roi_dir, num_frames_2fps=args.num_in)
    b15 = bboxes_2fps_to_15fps(b2, fps_in=args.fps_in, fps_out=args.fps_out, duration_s=args.duration)

    maps = []
    for bbox in b15:
        mw, mh, vals = bbox_to_qp_delta_map(
            bbox=bbox,
            width=args.width,
            height=args.height,
            ctu=args.ctu,
            gamma=args.gamma,
            base_qp=args.base_qp,
        )
        maps.append((mw, mh, vals))

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    write_kvazaar_roi_txt(out_path, maps)
    print(f"[OK] Wrote roi file: {out_path}")
    print(f"     Frames: {len(maps)}, Map: {maps[0][0]}x{maps[0][1]} (CTUs), base_qp={args.base_qp}, gamma={args.gamma}")


if __name__ == "__main__":
    main()
