#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import csv
import os
import subprocess
import sys


def parse_start_time(raw):
    value = float(str(raw).strip())
    return value, str(int(value)), f"{value:.1f}"


def run_ffmpeg(
    *,
    sample_id,
    start_time,
    video_root,
    baseline_root,
    fps,
    duration,
    start_number,
    overwrite,
):
    start_time_val, start_time_int, _ = parse_start_time(start_time)
    in_mp4 = os.path.join(video_root, f"sample_{sample_id}", "video.mp4")
    out_dir = os.path.join(
        baseline_root,
        f"sample_{sample_id}",
        f"video_{start_time_int}s_png_{fps}fps",
    )

    if not os.path.exists(in_mp4):
        print(f"[SKIP] Missing video: {in_mp4}", file=sys.stderr)
        return 1

    os.makedirs(out_dir, exist_ok=True)

    output_pattern = os.path.join(out_dir, "%06d.png")
    if overwrite:
        ffmpeg_overwrite = "-y"
    else:
        ffmpeg_overwrite = "-n"

    cmd = [
        "ffmpeg",
        ffmpeg_overwrite,
        "-hide_banner",
        "-loglevel",
        "error",
        "-ss",
        str(start_time_val),
        "-i",
        in_mp4,
        "-t",
        str(duration),
        "-vf",
        f"fps={fps}",
        "-start_number",
        str(start_number),
        "-vsync",
        "0",
        output_pattern,
    ]

    try:
        subprocess.run(cmd, check=True)
        print(f"[OK] sample_id={sample_id} start_time={start_time_int}s -> {out_dir}")
        return 0
    except subprocess.CalledProcessError:
        print(f"[ERR] ffmpeg failed: sample_id={sample_id}, start_time={start_time}", file=sys.stderr)
        return 1


def parse_args():
    parser = argparse.ArgumentParser(
        description="Extract source frame sequences for ROI encoding."
    )
    parser.add_argument("--csv", required=True, help="CSV with sample_id and start_time columns.")
    parser.add_argument("--video_root", required=True, help="StreamingBench video root containing sample_<id>/video.mp4.")
    parser.add_argument("--baseline_root", required=True, help="Output root for source frame folders.")
    parser.add_argument("--fps", type=int, default=30, help="Output frame rate.")
    parser.add_argument("--duration", type=float, default=5.0, help="Clip duration in seconds.")
    parser.add_argument("--start_number", type=int, default=1, help="First output frame number.")
    parser.add_argument("--overwrite", action="store_true", help="Overwrite existing frames.")
    return parser.parse_args()


def main():
    args = parse_args()
    if not os.path.exists(args.csv):
        raise FileNotFoundError(f"CSV not found: {args.csv}")

    with open(args.csv, "r", newline="", encoding="utf-8-sig") as f:
        reader = csv.DictReader(f)
        required = {"sample_id", "start_time"}
        missing = required - set(reader.fieldnames or [])
        if missing:
            raise ValueError(f"CSV missing columns: {missing}. got: {reader.fieldnames}")

        failures = 0
        for row_idx, row in enumerate(reader, start=2):
            sample_id = (row.get("sample_id") or "").strip()
            start_time = (row.get("start_time") or "").strip()
            if not sample_id or not start_time:
                print(f"[SKIP] Empty fields at line {row_idx}", file=sys.stderr)
                continue

            try:
                ret = run_ffmpeg(
                    sample_id=sample_id,
                    start_time=start_time,
                    video_root=args.video_root,
                    baseline_root=args.baseline_root,
                    fps=args.fps,
                    duration=args.duration,
                    start_number=args.start_number,
                    overwrite=args.overwrite,
                )
            except ValueError:
                print(f"[SKIP] Invalid start_time at line {row_idx}: {start_time}", file=sys.stderr)
                ret = 1

            failures += 1 if ret != 0 else 0

    if failures:
        raise SystemExit(f"Done with {failures} failures.")
    print("Done.")


if __name__ == "__main__":
    main()
