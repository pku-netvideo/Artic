#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import csv
import sys
import argparse
import subprocess
from dataclasses import dataclass
from typing import Optional, Dict, List, Tuple


# Encoding defaults.
SAMPLE_FPS = 30
START_IMG_IDX = 1
TARGET_SECONDS = 5
TARGET_FRAME_COUNT = SAMPLE_FPS * TARGET_SECONDS  # 150


@dataclass
class TaskResult:
    row_i: int
    sample_id: str
    original_index: str
    start_time_int: str
    start_time_1dp: str
    res_tag: str
    status: str           # OK / SKIP / WARN / FAIL
    message: str
    output: Optional[str] = None


def short_err(s: str, max_lines: int = 20) -> str:
    """Return only the tail of a command error to keep logs readable."""
    if not s:
        return ""
    lines = s.strip().splitlines()
    if len(lines) <= max_lines:
        return "\n".join(lines)
    return "\n".join(lines[-max_lines:])


def run_cmd(cmd, *, dry_run=False, verbose=False, tail_lines=25):
    """Run a subprocess command.

    In verbose mode output is streamed directly. Otherwise output is captured
    and only the tail of stderr is printed when a command fails.
    """
    cmd_str = " ".join(map(str, cmd))
    if verbose or dry_run:
        print(cmd_str)

    if dry_run:
        return None

    if verbose:
        subprocess.run(cmd, check=True)
        return None

    try:
        p = subprocess.run(
            cmd,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        return p
    except subprocess.CalledProcessError as e:
        print(f"[CMD FAIL] {cmd_str}")
        if e.stderr:
            print("---- stderr (tail) ----")
            print(short_err(e.stderr, max_lines=tail_lines))
            print("-----------------------")
        raise


def parse_start_time(raw):
    """Parse start_time into both path formats used by this pipeline.

    ROI folders use one decimal place, while source-frame folders use integer
    seconds.
    """
    s = str(raw).strip()
    if s == "":
        raise ValueError("empty start_time")

    try:
        f = float(s)
    except Exception as e:
        raise ValueError(f"start_time is not numeric: {s}") from e

    start_time_int = int(f)
    start_time_1dp = f"{f:.1f}"
    return str(start_time_int), start_time_1dp


def build_roi_dir(roi_root, sample_id, start_time_1dp, original_index):
    return os.path.join(
        roi_root,
        f"sample_{sample_id}_start_{start_time_1dp}s",
        f"q_{original_index}"
    )


def build_sample_dir(baseline_root, sample_id):
    return os.path.join(baseline_root, f"sample_{sample_id}")


def build_frames_dir(sample_dir, start_time_int, encode_fps):
    return os.path.join(sample_dir, f"video_{start_time_int}s_png_{encode_fps}fps")


def format_fps_tag(fps: int) -> str:
    return f"{fps}fps"


def parse_resolution(resolution_str: str) -> Tuple[int, int]:
    """Parse the shared target resolution in WIDTHxHEIGHT form."""
    s = resolution_str.strip().lower()
    if not s:
        raise ValueError("empty resolution")

    if "x" not in s:
        raise ValueError(f"bad resolution: {s} (expect WxH)")

    w_str, h_str = s.split("x", 1)
    w = int(w_str)
    h = int(h_str)
    if w <= 0 or h <= 0:
        raise ValueError(f"bad resolution: {s} (width/height must be positive)")

    return w, h


def parse_bitrate_one(bitrate_str: str) -> Dict[str, object]:
    s = bitrate_str.strip().lower()
    if not s:
        raise ValueError("empty bitrate")

    kbps_str = s[:-1] if s.endswith("k") else s
    kbps = int(kbps_str)
    if kbps <= 0:
        raise ValueError(f"bad bitrate: {s} (kbps must be positive)")

    return dict(res_tag=f"{kbps}k", target_bitrate_kbps=kbps)


def flatten_bitrates(values: List[List[str]]) -> List[str]:
    return [item for group in values for item in group]


def encode_one_resolution_roi(
    *,
    roi_root,
    baseline_root,
    kvazaar_bin,
    gen_script,
    gamma,
    sample_id,
    start_time_int,
    start_time_1dp,
    original_index,
    res_tag,
    target_width,
    target_height,
    target_bitrate_kbps,
    encode_fps,
    target_seconds,
    start_img_idx,
    tmp_dir="/tmp",
    tmp_prefix="kv_tmp",
    dry_run=False,
    keep_temp=False,
    verbose=False
) -> TaskResult:

    sample_dir = build_sample_dir(baseline_root, sample_id)
    os.makedirs(sample_dir, exist_ok=True)

    roi_txt_path = os.path.join(sample_dir, f"roi_{format_fps_tag(encode_fps)}_q_{original_index}.txt")

    frames_dir = build_frames_dir(sample_dir, start_time_int, encode_fps)
    target_frame_count = int(round(encode_fps * target_seconds))

    output_mp4_path = os.path.join(
        sample_dir,
        f"video_{res_tag}_{start_time_int}s_q_{original_index}_roi.mp4"
    )

    # Missing inputs are reported as WARN so the batch can continue.
    if not os.path.isdir(frames_dir):
        return TaskResult(
            row_i=-1, sample_id=sample_id, original_index=original_index,
            start_time_int=start_time_int, start_time_1dp=start_time_1dp,
            res_tag=res_tag, status="WARN",
            message=f"frames_dir not found: {frames_dir}"
        )

    roi_dir = build_roi_dir(roi_root, sample_id, start_time_1dp, original_index)
    if not os.path.isdir(roi_dir):
        return TaskResult(
            row_i=-1, sample_id=sample_id, original_index=original_index,
            start_time_int=start_time_int, start_time_1dp=start_time_1dp,
            res_tag=res_tag, status="WARN",
            message=f"roi_dir not found: {roi_dir}"
        )

    # Generate the Kvazaar ROI QP map shared by all bitrate outputs.
    gen_cmd = [
        sys.executable, gen_script,
        "--roi_root", roi_root,
        "--baseline_root", baseline_root,
        "--sample_id", sample_id,
        "--start_time", start_time_1dp,
        "--original_index", original_index,
        "--width", str(target_width),
        "--height", str(target_height),
        "--gamma", str(gamma),
        "--fps_out", str(encode_fps),
        "--duration", str(target_seconds),
    ]

    # Temporary files for raw YUV and encoded HEVC bitstream.
    # tmp_prefix = os.path.join(sample_dir, f"tmp_{res_tag}_{start_time_int}s_q_{original_index}_roi")
    # temp_yuv_path = tmp_prefix + ".yuv"
    # temp_hevc_path = tmp_prefix + ".hevc"
    tmp_prefix = os.path.join(tmp_dir, f"{tmp_prefix}_{res_tag}")
    temp_yuv_path = tmp_prefix + ".yuv"
    temp_hevc_path = tmp_prefix + ".hevc"

    # Convert shared PNG frames to raw YUV at the target resolution.
    ffmpeg_gen_yuv_cmd = [
        "ffmpeg", "-y",
        "-hide_banner", "-loglevel", "error",
        "-framerate", str(encode_fps),
        "-start_number", str(start_img_idx),
        "-i", os.path.join(frames_dir, "%06d.png"),
        "-vf", f"scale={target_width}:{target_height}",
        "-frames:v", str(target_frame_count),
        "-f", "rawvideo",
        "-pix_fmt", "yuv420p",
        temp_yuv_path
    ]

    # Encode with Kvazaar using the generated ROI QP map.
    kvazaar_cmd = [
        kvazaar_bin,
        "-i", temp_yuv_path,
        "--input-res", f"{target_width}x{target_height}",
        "--input-fps", str(encode_fps),
        "--gop", "0",
        "--period", "0",
        "--frames", str(target_frame_count),
        "--bitrate", str(target_bitrate_kbps * 1000),
        "--roi", roi_txt_path,
        "-o", temp_hevc_path,
    ]

    mux_cmd = [
        "ffmpeg", "-y",
        "-hide_banner", "-loglevel", "error",
        "-r", str(encode_fps),
        "-i", temp_hevc_path,
        "-c:v", "copy",
        "-tag:v", "hvc1",
        "-movflags", "+faststart",
        output_mp4_path
    ]

    try:
        run_cmd(gen_cmd, dry_run=dry_run, verbose=verbose)
        run_cmd(ffmpeg_gen_yuv_cmd, dry_run=dry_run, verbose=verbose)
        run_cmd(kvazaar_cmd, dry_run=dry_run, verbose=verbose)
        run_cmd(mux_cmd, dry_run=dry_run, verbose=verbose)

        # if not dry_run and not keep_temp:
        #     for p in [temp_yuv_path, temp_hevc_path]:
        #         try:
        #             os.remove(p)
        #         except FileNotFoundError:
        #             pass

        return TaskResult(
            row_i=-1, sample_id=sample_id, original_index=original_index,
            start_time_int=start_time_int, start_time_1dp=start_time_1dp,
            res_tag=res_tag, status="OK",
            message="encoded", output=output_mp4_path
        )

    except Exception as e:
        return TaskResult(
            row_i=-1, sample_id=sample_id, original_index=original_index,
            start_time_int=start_time_int, start_time_1dp=start_time_1dp,
            res_tag=res_tag, status="FAIL",
            message=str(e)
        )


def print_progress(done: int, total: int, stats: Dict[str, int], prefix: str = ""):
    stat_str = " ".join([f"{k}:{v}" for k, v in stats.items()])
    print(f"{prefix}{done}/{total}  {stat_str}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv_path", required=True, help="Input CSV path.")
    ap.add_argument("--roi_root", required=True, help="Root folder containing ROI/sample_<id>_start_<time>s/q_<original_index>.")
    ap.add_argument("--baseline_root", required=True, help="Root folder containing baseline/sample_<id> frame folders and outputs.")
    ap.add_argument("--kvazaar_bin", required=True, help="Path to the kvazaar executable, e.g. /usr/local/bin/kvazaar.")
    ap.add_argument("--gen_script", default="gen_kvazaar_roi.py", help="Path to the ROI QP-map generation script.")
    ap.add_argument("--gamma", type=float, default=2.0, help="ROI falloff gamma.")
    ap.add_argument("--dry_run", action="store_true", help="Print commands without running them.")
    ap.add_argument("--keep_temp", action="store_true", help="Keep temporary yuv/hevc files.")
    ap.add_argument("--verbose", action="store_true", help="Stream detailed ffmpeg/kvazaar output.")

    ap.add_argument("--encode_fps", type=int, default=SAMPLE_FPS, help="Encoding FPS and ROI-map FPS. Must match the shared source frame folder suffix.")
    ap.add_argument("--target_seconds", type=float, default=TARGET_SECONDS, help="Encoding duration in seconds.")
    ap.add_argument("--start_img_idx", type=int, default=START_IMG_IDX, help="First source frame index read by ffmpeg.")
    ap.add_argument("--tmp_dir", default="/tmp", help="Temporary file directory. Prefer a local disk.")
    ap.add_argument("--tmp_prefix", default="kv_tmp", help="Temporary filename prefix.")
    ap.add_argument("--start_row", type=int, default=1, help="First CSV data row to process, 1-based and inclusive.")
    ap.add_argument("--end_row", type=int, default=-1, help="Last CSV data row to process, 1-based and inclusive. -1 means all rows.")

    ap.add_argument("--resolution", default="640x360", help="Unified target resolution for all bitrate encodes, e.g. 640x360.")
    ap.add_argument(
        "--bitrate",
        nargs="+",
        action="append",
        default=[],
        help="Target bitrate(s) in kbps. The output tag is derived from this value, e.g. 200 -> 200k."
    )

    args = ap.parse_args()

    if not os.path.isfile(args.csv_path):
        raise FileNotFoundError(f"CSV not found: {args.csv_path}")
    if not os.path.isfile(args.gen_script):
        raise FileNotFoundError(f"gen_script not found: {args.gen_script}")

    target_width, target_height = parse_resolution(args.resolution)

    bitrate_values = flatten_bitrates(args.bitrate)
    if not bitrate_values:
        bitrate_values = ["200"]

    jobs: List[Dict[str, object]] = [parse_bitrate_one(b) for b in bitrate_values]

    # Read the CSV once so progress totals reflect only valid rows.
    rows: List[Tuple[int, Dict[str, str], str, str, str, str]] = []
    with open(args.csv_path, "r", encoding="utf-8-sig") as f:
        reader = csv.DictReader(f)
        required_cols = {"original_index", "sample_id", "start_time"}
        missing = required_cols - set(reader.fieldnames or [])
        if missing:
            raise ValueError(f"CSV missing columns: {missing}. got: {reader.fieldnames}")

        for row_i, row in enumerate(reader, start=1):
            original_index = str(row["original_index"]).strip()
            sample_id = str(row["sample_id"]).strip()
            raw_start_time = row["start_time"]

            if row_i < args.start_row:
                continue
            if args.end_row != -1 and row_i > args.end_row:
                continue

            if original_index == "" or sample_id == "":
                continue

            try:
                start_time_int, start_time_1dp = parse_start_time(raw_start_time)
            except Exception:
                continue

            rows.append((row_i, row, original_index, sample_id, start_time_int, start_time_1dp))

    total = len(rows) * len(jobs)
    done = 0
    stats = {"OK": 0, "SKIP": 0, "WARN": 0, "FAIL": 0}
    results: List[TaskResult] = []

    print(f"[INFO] valid rows: {len(rows)}; total tasks (rows*bitrates): {total}")
    print(f"[INFO] resolution: {target_width}x{target_height}")
    print("[INFO] bitrates:")
    for j in jobs:
        print(f"  - {j['res_tag']}: {j['target_bitrate_kbps']} kbps")

    for idx, (row_i, row, original_index, sample_id, start_time_int, start_time_1dp) in enumerate(rows, start=1):
        print(f"\n===== Row {row_i} ({idx}/{len(rows)}): sample_id={sample_id}, start_time={start_time_int}s/{start_time_1dp}s, q={original_index} =====")

        for j in jobs:
            res_tag = str(j["res_tag"])
            print(f"--> [{res_tag}] processing...")

            r = encode_one_resolution_roi(
                roi_root=args.roi_root,
                baseline_root=args.baseline_root,
                kvazaar_bin=args.kvazaar_bin,
                gen_script=args.gen_script,
                gamma=args.gamma,
                sample_id=sample_id,
                start_time_int=start_time_int,
                start_time_1dp=start_time_1dp,
                original_index=original_index,
                res_tag=res_tag,
                target_width=target_width,
                target_height=target_height,
                target_bitrate_kbps=int(j["target_bitrate_kbps"]),
                encode_fps=args.encode_fps,
                target_seconds=args.target_seconds,
                start_img_idx=args.start_img_idx,
                tmp_dir=args.tmp_dir,
                tmp_prefix=args.tmp_prefix,
                dry_run=args.dry_run,
                keep_temp=args.keep_temp,
                verbose=args.verbose
            )

            r.row_i = row_i
            results.append(r)

            stats[r.status] = stats.get(r.status, 0) + 1
            done += 1

            if r.status == "OK":
                print(f"[OK]   {res_tag} -> {r.output}")
            elif r.status == "SKIP":
                print(f"[SKIP] {res_tag} -> {r.message}")
            elif r.status == "WARN":
                print(f"[WARN] {res_tag} -> {r.message}")
            else:
                print(f"[FAIL] {res_tag} -> {r.message}")

            print_progress(done, total, stats, prefix="Progress: ")

    print("\n========== SUMMARY ==========")
    print(f"Total tasks: {total}")
    for k in ["OK", "SKIP", "WARN", "FAIL"]:
        print(f"{k}: {stats.get(k, 0)}")

    bad = [x for x in results if x.status in ("WARN", "FAIL")]
    if bad:
        print("\n---- WARN/FAIL tasks ----")
        for x in bad:
            print(f"Row {x.row_i} sample={x.sample_id} q={x.original_index} t={x.start_time_int}s res={x.res_tag} [{x.status}] {x.message}")
    print("=============================\n")


if __name__ == "__main__":
    main()
