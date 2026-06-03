import os
import re
import csv
import json
import time
import base64
import argparse
import asyncio
from dataclasses import dataclass
from typing import List, Optional, Tuple, Any, Dict

from volcenginesdkarkruntime import Ark

# -----------------------------
# Utils
# -----------------------------
def format_start_time_one_decimal(v: Any) -> str:
    try:
        return f"{float(v):.1f}"
    except Exception:
        return f"{v}"


def b64_encode_image(path: str) -> str:
    with open(path, "rb") as f:
        return base64.b64encode(f.read()).decode("utf-8")


def normalize_bbox(b: Any) -> Optional[List[List[float]]]:
    """Normalize supported bbox formats to [[x1, y1], [x2, y2]].

    Accepted inputs include [[x1,y1],[x2,y2]], [x1,y1,x2,y2], and
    [[x1,y1,x2,y2]].
    """
    try:
        if isinstance(b, list) and len(b) == 1 and isinstance(b[0], list) and len(b[0]) == 4:
            b = b[0]

        if isinstance(b, list) and len(b) == 4 and all(isinstance(x, (int, float)) for x in b):
            x1, y1, x2, y2 = map(float, b)
            return [[x1, y1], [x2, y2]]

        if (
            isinstance(b, list) and len(b) == 2
            and isinstance(b[0], list) and isinstance(b[1], list)
            and len(b[0]) == 2 and len(b[1]) == 2
        ):
            x1, y1 = map(float, b[0])
            x2, y2 = map(float, b[1])
            return [[x1, y1], [x2, y2]]
    except Exception:
        return None

    return None


def parse_model_output(text: str, pred_steps: int) -> Tuple[Optional[List[List[List[float]]]], str]:
    """Parse model output into predicted boxes and a parse mode.

    Returns (result_or_none, parse_mode), where parse_mode is json, regex, or
    fail.
    """
    if not isinstance(text, str):
        return None, "fail"

    clean = text.replace("```json", "").replace("```", "").strip()

    s = clean.find("{")
    e = clean.rfind("}")
    if s != -1 and e != -1 and e > s:
        json_str = clean[s:e+1]
        try:
            data = json.loads(json_str)
            res = data.get("result", None)
            if isinstance(res, list) and len(res) == pred_steps:
                norm = []
                for b in res:
                    nb = normalize_bbox(b)
                    if nb is None:
                        return None, "fail"
                    norm.append(nb)
                return norm, "json"
        except Exception:
            pass

    nums = re.findall(r"-?\d+\.?\d*", clean)
    need = pred_steps * 4
    if len(nums) >= need:
        tail = nums[-need:]
        try:
            vals = [float(x) for x in tail]
            out = []
            for i in range(0, len(vals), 4):
                x1, y1, x2, y2 = vals[i:i+4]
                out.append([[x1, y1], [x2, y2]])
            return out, "regex"
        except Exception:
            return None, "fail"

    return None, "fail"


def build_prompt(question: str, pred_steps: int) -> str:
    output_example = ",\n".join([f"[[x_min_{i}, y_min_{i}], [x_max_{i}, y_max_{i}]]" for i in range(1, pred_steps + 1)])
    
    prompt = f"""# Role
You are an expert in computer vision and object tracking.

# Task
Your task is to analyze the provided sequence of video frames and **predict the bounding boxes of the relative region of the question:{question} for the next {pred_steps} consecutive frames** (following the end of the sequence).
For example, if the question is "What is the text on the white truck", you need to track the white truck in the historical frames and predict its bounding boxes in the next {pred_steps} frames.

# Coordinate System
- Top-left corner: (0, 0)
- Bottom-right corner: (1000, 1000)

# Chain of Thought Process
Before providing the final coordinates, please follow these steps:
1.  **Contextual Reasoning:** Analyze the scene and events.
2.  **Detection:** Locate the relative region of question: {question} in the provided historical frames.
3.  **Motion Analysis:** Calculate the velocity and trajectory of the target based on the history.
    * Determine direction and speed (pixels per frame).
    * Analyze acceleration or deceleration.
4.  **Trajectory Projection:** Based on the motion analysis, extrapolate the path for the next {pred_steps} frames.
    * Predict position for t+1.
    * Predict position for t+2 based on t+1, and so on.
    * Adjust for any potential collisions or scene changes observed.

# Output Format
You must output a **single Valid JSON object** containing exactly two keys: "reasoning" and "result".

1. **"reasoning"**: A string containing your detailed step-by-step analysis (Context, Detection, Motion, Projection).
2. **"result"**: A list of lists containing the predicted bounding boxes for the next {pred_steps} frames, coordinate range is 0 to 1000.
If there are no valid bounding boxes to predict, please return [[0,0],[0,0]] for each frame.

**JSON Structure Example:**
{{
  "reasoning": "The object is moving at a constant speed of...",
  "result": [
    {output_example}
  ]
}}"""
    return prompt


def build_frame_index(folder: str) -> Dict[int, str]:
    """Build a mapping from numeric frame id to image path.

    Expected names are zero-padded frame ids such as 00001.png.
    """
    exts = (".png", ".jpg", ".jpeg", ".webp", ".bmp")
    out: Dict[int, str] = {}
    for name in os.listdir(folder):
        low = name.lower()
        if not low.endswith(exts):
            continue
        stem = os.path.splitext(name)[0]
        if not stem.isdigit():
            continue
        fid = int(stem)
        out[fid] = os.path.join(folder, name)
    return out


# -----------------------------
# Job + Predictor
# -----------------------------
@dataclass
class Job:
    sample_id: str
    start_time_str: str
    folder: str
    question: str
    a: int                 # 1-based input start frame id.
    history_ids: List[int] # History frame ids.
    future_ids: List[int]  # Future frame ids.
    history_paths: List[str]
    question_id: Optional[int] = None  # Optional row id for debugging.


class RoiPredictor:
    def __init__(
        self,
        api_key: str,
        model: str,
        base_url: str,
        pred_steps: int,
        max_retry: int,
        concurrency: int,
        thinking_enabled: bool = True,
    ):
        self.client = Ark(base_url=base_url, api_key=api_key)
        self.model = model
        self.pred_steps = pred_steps
        self.max_retry = max_retry
        self.sem = asyncio.Semaphore(concurrency)
        self.thinking_enabled = thinking_enabled

        self._b64_cache: Dict[str, str] = {}

    def _get_b64(self, path: str) -> str:
        if path in self._b64_cache:
            return self._b64_cache[path]
        b = b64_encode_image(path)
        if len(self._b64_cache) > 5000:
            self._b64_cache.clear()
        self._b64_cache[path] = b
        return b

    def _call_api_sync(self, history_paths: List[str], question: str) -> str:
        content = []
        for p in history_paths:
            img_b64 = self._get_b64(p)
            content.append({
                "type": "input_image",
                "image_url": f"data:image/png;base64,{img_b64}",
            })
        content.append({
            "type": "input_text",
            "text": build_prompt(question, self.pred_steps)
        })
 
        resp = self.client.responses.create(
            model=self.model,
            input=[{"role": "user", "content": content}],
            thinking={"type": "enabled"} if self.thinking_enabled else {"type": "disabled"},
        )
        # print("calling API..., got response.")
        return resp.output[-1].content[0].text

    async def predict_with_retry(self, job: Job) -> Dict[str, Any]:
        async with self.sem:
            last_err = None
            for attempt in range(1, self.max_retry + 1):
                try:
                    text = await asyncio.to_thread(self._call_api_sync, job.history_paths, job.question)
                    parsed, mode = parse_model_output(text, self.pred_steps)
                    if parsed is not None:
                        print("get response,parse success.")
                        return {
                            "ok": True,
                            "parse_mode": mode,
                            "result": parsed,
                            "raw": text,
                            "attempt": attempt,
                        }
                    last_err = f"parse_failed(mode={mode})"
                    print(f"Attempt {attempt}: parse failed, raw response: {text}")
                except Exception as e:
                    last_err = repr(e)

                await asyncio.sleep(min(1.5 * attempt, 6.0))

            return {"ok": False, "error": last_err or "unknown_error", "attempt": self.max_retry}


# -----------------------------
# Worker + Pipeline
# -----------------------------
async def worker_loop(
    q: asyncio.Queue,
    predictor: RoiPredictor,
    stats: dict,
    lock: asyncio.Lock,
    save_raw: bool,
):
    while True:
        job = await q.get()
        if job is None:
            q.task_done()
            break

        out = await predictor.predict_with_retry(job)

        # save_name = f"pred_{job.a - 1:05d}.json"
        subdir = os.path.join(job.folder, f"q_{job.question_id}")
        os.makedirs(subdir, exist_ok=True)
        save_path = os.path.join(subdir, f"pred_{job.a - 1:05d}.json")

        payload = {
            "sample_id": job.sample_id,
            "start_time": job.start_time_str,
            "a": job.a,  # 1-based
            "history_frames": [f"{i:05d}" for i in job.history_ids],
            "future_frames": [f"{i:05d}" for i in job.future_ids],
            "question": job.question,
            "ok": out.get("ok", False),
        }

        if out.get("ok"):
            payload["parse_mode"] = out.get("parse_mode")
            payload["result"] = out.get("result")
            payload["attempt"] = out.get("attempt")
            if save_raw:
                payload["raw"] = out.get("raw")
        else:
            payload["error"] = out.get("error")
            payload["attempt"] = out.get("attempt")

        with open(save_path, "w", encoding="utf-8") as f:
            json.dump(payload, f, ensure_ascii=False, indent=2)
            print(f"[DONE] {job.folder} q_{job.question_id} pred_{job.a-1:05d} ok={payload['ok']}")

        async with lock:
            stats["done"] += 1
            stats["ok"] += 1 if payload["ok"] else 0
            stats["fail"] += 0 if payload["ok"] else 1
            if stats["done"] % 10 == 0:
                print(f"[progress] done={stats['done']} ok={stats['ok']} fail={stats['fail']}")

        q.task_done()


async def main_async(args):
    predictor = RoiPredictor(
        api_key=args.api_key,
        model=args.model,
        base_url=args.base_url,
        pred_steps=args.pred,
        max_retry=args.max_retry,
        concurrency=args.concurrency,
        thinking_enabled=not args.disable_thinking,
    )

    q: asyncio.Queue = asyncio.Queue(maxsize=args.queue_size)
    stats = {"done": 0, "ok": 0, "fail": 0, "enqueued": 0, "skipped": 0}
    lock = asyncio.Lock()

    workers = [
        asyncio.create_task(worker_loop(q, predictor, stats, lock, args.save_raw))
        for _ in range(args.concurrency)
    ]

    with open(args.csv, "r", encoding="utf-8-sig") as f:
        reader = csv.DictReader(f)
        print("CSV fieldnames:", reader.fieldnames)
        for row_idx, row in enumerate(reader):
            sample_id = str(row.get("sample_id", "")).strip()
            question = str(row.get("question", "")).strip()
            start_time_str = format_start_time_one_decimal(row.get("start_time", "0"))
            if not sample_id or not question:
                async with lock:
                    stats["skipped"] += 1
                continue

            folder = os.path.join(args.roi_root, f"sample_{sample_id}_start_{start_time_str}s")
            # print(folder)
            if not os.path.isdir(folder):
                async with lock:
                    stats["skipped"] += 1
                continue

            frame_map = build_frame_index(folder)

            # Each ROI folder is expected to contain 00001..frames_expected.
            needed_ids = list(range(1, args.frames_expected + 1))
            if any(fid not in frame_map for fid in needed_ids):
                async with lock:
                    stats["skipped"] += 1
                continue

            # Limit sliding windows so history and future frames both exist.
            max_a = args.frames_expected - (args.history + args.pred) + 1  # e.g. 16-6+1=11
            # Build 1-based sliding windows over history and future frames.
            for a in range(args.a_start, max_a + 1):
                history_ids = [a + i for i in range(args.history)]
                future_ids = [a + args.history + i for i in range(args.pred)]
                history_paths = [frame_map[i] for i in history_ids]

                job = Job(
                    sample_id=sample_id,
                    start_time_str=start_time_str,
                    folder=folder,
                    question=question,
                    a=a,
                    history_ids=history_ids,
                    future_ids=future_ids,
                    history_paths=history_paths,
                    question_id=row_idx,
                )
                await q.put(job)
                async with lock:
                    stats["enqueued"] += 1

            if (row_idx + 1) % 20 == 0:
                async with lock:
                    print(f"[enqueue] rows={row_idx+1} enqueued={stats['enqueued']} skipped={stats['skipped']}")

    for _ in range(args.concurrency):
        await q.put(None)

    await q.join()
    for w in workers:
        await w

    print(f"[final] enqueued={stats['enqueued']} done={stats['done']} ok={stats['ok']} fail={stats['fail']} skipped_rows={stats['skipped']}")


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--csv", required=True, help="CSV containing sample_id, question, and start_time.")
    p.add_argument("--roi-root", required=True, help="Root folder containing sampled frames.")
    p.add_argument("--frames-expected", type=int, default=16, help="Number of sampled frames per sample, e.g. 00001..00016.")
    p.add_argument("--history", type=int, default=3)
    p.add_argument("--pred", type=int, default=3)

    p.add_argument("--a-start", type=int, default=2, help="Sliding-window start frame id, 1-based.")

    p.add_argument("--concurrency", type=int, default=16)
    p.add_argument("--queue-size", type=int, default=256)
    p.add_argument("--max-retry", type=int, default=3)

    p.add_argument("--base-url", default="https://ark.cn-beijing.volces.com/api/v3")
    p.add_argument("--model", default="doubao-seed-1-8-251228")
    p.add_argument("--api-key", default=os.environ.get("ARK_API_KEY", ""), help="Ark API key. Defaults to ARK_API_KEY.")

    p.add_argument("--save-raw", action="store_true", help="Save raw model output; this increases output size.")
    p.add_argument("--disable-thinking", action="store_true", help="Disable model thinking to reduce output variability.")
    return p.parse_args()


def main():
    args = parse_args()
    if not args.api_key:
        raise SystemExit("ERROR: missing --api-key or env ARK_API_KEY")

    t0 = time.time()
    asyncio.run(main_async(args))
    print(f"Elapsed: {time.time() - t0:.1f}s")


if __name__ == "__main__":
    main()
