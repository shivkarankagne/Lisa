#!/usr/bin/env python3
# SPDX-License-Identifier: BUSL-1.1
"""Run the W0/W12 benchmark against LISA and score it.

    benchmark/w0/run_lisa.py [--bin build/lisa] [--corpus benchmark/w0/corpus]
                             [--out benchmark/w0/results-lisa.json] [--skip-ingest]

Measures: indexing time and peak memory, answer time per question, and
for each question whether the answer contains the expected fact and
cites the document it came from. Unanswerable questions must be refused.
Everything runs locally; nothing is uploaded.
"""

import argparse
import json
import os
import pathlib
import re
import resource
import shutil
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent

ap = argparse.ArgumentParser()
ap.add_argument("--bin", default=str(ROOT / "build" / "lisa"))
ap.add_argument("--corpus", default=str(HERE / "corpus"))
ap.add_argument("--data", default=str(HERE / "data-lisa"))
ap.add_argument("--out", default=str(HERE / "results-lisa.json"))
ap.add_argument("--questions", default=str(HERE / "questions.json"))
ap.add_argument("--collection", default="bench")
ap.add_argument("--skip-ingest", action="store_true")
args = ap.parse_args()

spec = json.loads(pathlib.Path(args.questions).read_text(encoding="utf-8"))
questions = spec["questions"]
results = {"tool": "lisa", "corpus": args.corpus, "questions": []}


def run(cmd, **kw):
    before = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    t0 = time.monotonic()
    p = subprocess.run(cmd, capture_output=True, text=True, **kw)
    elapsed = time.monotonic() - t0
    peak = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    return p, elapsed, max(peak, before) / (1024 * 1024)   # macOS reports bytes


def corpus_stats(path):
    files = [p for p in pathlib.Path(path).rglob("*") if p.is_file()]
    return len(files), sum(p.stat().st_size for p in files)


n_files, n_bytes = corpus_stats(args.corpus)
results["corpus_files"] = n_files
results["corpus_bytes"] = n_bytes
print(f"corpus: {n_files} files, {n_bytes / 1e6:.1f} MB")

# ---- index ---------------------------------------------------------------------
if not args.skip_ingest:
    shutil.rmtree(args.data, ignore_errors=True)
    print("indexing...", flush=True)
    p, elapsed, peak = run([args.bin, "ingest", "--data", args.data,
                            "--collection", args.collection, args.corpus, "--json"])
    if p.returncode != 0:
        sys.exit(f"ingest failed: {p.stderr[-2000:]}")
    st = json.loads(p.stdout)
    results["index"] = {"seconds": round(elapsed, 1), "peak_rss_mb": round(peak, 1), **st}
    disk = sum(f.stat().st_size for f in pathlib.Path(args.data).rglob("*") if f.is_file())
    results["index"]["index_bytes"] = disk
    print(f"  {st['files_added']} files, {st['chunks_added']} chunks, {elapsed:.1f}s, "
          f"peak {peak:.0f} MB, index {disk / 1e6:.1f} MB")

# ---- ask -----------------------------------------------------------------------
ok_answer = ok_citation = ok_refusal = 0
answerable = sum(1 for q in questions if not q.get("unanswerable"))
unanswerable = len(questions) - answerable

for q in questions:
    p, elapsed, _ = run([args.bin, "ask", "--data", args.data, "--collection", args.collection,
                         "--json", q["q"]])
    row = {"id": q["id"], "q": q["q"], "seconds": round(elapsed, 1)}
    if p.returncode != 0:
        row["error"] = p.stderr.strip()[-300:]
        results["questions"].append(row)
        print(f"  {q['id']:>2}. ERROR {row['error'][:80]}")
        continue
    a = json.loads(p.stdout)
    text = a["text"]
    cited = [os.path.basename(c["path"]) for c in a["citations"]]
    row.update({"found": a["found"], "answer": text, "cited": cited,
                "prompt_tokens": a["prompt_tokens"], "first_token_seconds": a["first_token_seconds"]})

    if q.get("unanswerable"):
        row["correct"] = not a["found"]
        ok_refusal += bool(row["correct"])
    else:
        # The expected fact may be written with or without the thousands commas.
        def norm(s):
            return re.sub(r"[,\s]", "", s.lower())
        row["correct"] = any(norm(e) in norm(text) for e in q["expect"])
        row["cited_source"] = q["source"] in cited if q.get("source") else None
        ok_answer += bool(row["correct"])
        ok_citation += bool(row["cited_source"])
    results["questions"].append(row)
    mark = "ok " if row["correct"] else "MISS"
    cite = "" if q.get("unanswerable") else (" cite:ok" if row.get("cited_source") else " cite:MISS")
    print(f"  {q['id']:>2}. {mark}{cite} {elapsed:5.1f}s  {text[:90]}")

results["score"] = {
    "answerable": answerable,
    "answered_correctly": ok_answer,
    "cited_correct_source": ok_citation,
    "unanswerable": unanswerable,
    "refused_correctly": ok_refusal,
    "median_seconds": round(sorted(r["seconds"] for r in results["questions"])[len(questions) // 2], 1),
}
pathlib.Path(args.out).write_text(json.dumps(results, indent=2, ensure_ascii=False), encoding="utf-8")
s = results["score"]
print(f"\nanswers {s['answered_correctly']}/{answerable}, citations {s['cited_correct_source']}/{answerable}, "
      f"refusals {s['refused_correctly']}/{unanswerable}, median {s['median_seconds']}s")
print(f"written to {args.out}")
