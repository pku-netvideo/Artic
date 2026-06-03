#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import os
import subprocess

import pandas as pd


def extract_frames_ffmpeg(csv_path, base_video_dir, base_output_dir, target_fps=2.0):
    """Extract frames from sample videos according to sample_id/start_time in a CSV."""
    if not os.path.exists(csv_path):
        raise FileNotFoundError(f"CSV not found: {csv_path}")

    df = pd.read_csv(csv_path)
    required_columns = ["sample_id", "start_time"]
    missing = [c for c in required_columns if c not in df.columns]
    if missing:
        raise ValueError(f"CSV missing required columns: {missing}")

    print(f"Start processing {len(df)} samples.")

    for _, row in df.iterrows():
        sample_id = str(row["sample_id"])

        try:
            center_time = float(row["start_time"])
        except ValueError:
            print(f"[SKIP] sample={sample_id}: invalid start_time={row['start_time']}")
            continue

        video_path = os.path.join(base_video_dir, f"sample_{sample_id}", "video.mp4")
        output_dir = os.path.join(base_output_dir, f"sample_{sample_id}_start_{center_time:.1f}s")

        if not os.path.exists(video_path):
            print(f"[WARN] sample={sample_id}: video not found: {video_path}")
            continue

        start_sec = max(0, center_time - 3.0)
        end_sec = center_time + 5.0
        duration = end_sec - start_sec

        os.makedirs(output_dir, exist_ok=True)
        output_pattern = os.path.join(output_dir, "%05d.png")

        cmd = [
            "ffmpeg",
            "-y",
            "-ss",
            str(start_sec),
            "-t",
            str(duration),
            "-i",
            video_path,
            "-vf",
            f"fps={target_fps}",
            "-vsync",
            "0",
            output_pattern,
        ]

        try:
            subprocess.run(cmd, check=True)
            print(f"[OK] sample={sample_id}: {start_sec:.1f}s-{end_sec:.1f}s -> {output_dir}")
        except subprocess.CalledProcessError:
            print(f"[FAIL] sample={sample_id}: ffmpeg failed")

    print("Done.")


def parse_args():
    parser = argparse.ArgumentParser(
        description="Extract ROI candidate frames from source videos at a fixed FPS."
    )
    parser.add_argument("--csv", required=True, help="CSV file with sample_id and start_time columns.")
    parser.add_argument(
        "--base-video-dir",
        required=True,
        help="Root folder containing sample_<sample_id>/video.mp4.",
    )
    parser.add_argument(
        "--output-dir",
        required=True,
        help="Output root folder for sampled frames, e.g. /path/to/ROI.",
    )
    parser.add_argument("--target-fps", type=float, default=2.0, help="Frame sampling FPS.")
    return parser.parse_args()


if __name__ == "__main__":
    args = parse_args()
    extract_frames_ffmpeg(
        args.csv,
        args.base_video_dir,
        args.output_dir,
        target_fps=args.target_fps,
    )
