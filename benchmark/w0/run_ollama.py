#!/usr/bin/env python3
# SPDX-License-Identifier: BUSL-1.1
"""Ask the W0 questions of a plain local model through Ollama, with no
document search, to show what a model alone can and cannot answer.

    benchmark/w0/run_ollama.py [--model qwen3:4b] [--out results-ollama.json]

This is the floor of the comparison, not a rival product: Ollama runs
models, it does not read your folders. The same model (Qwen3 4B) is the
one LISA uses, so the difference in the scores is the retrieval, not the
model.
"""

import argparse
import json
import pathlib
import re
import time
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
ap = argparse.ArgumentParser()
ap.add_argument("--model", default="qwen3:4b")
ap.add_argument("--host", default="http://127.0.0.1:11434")
ap.add_argument("--questions", default=str(HERE / "questions.json"))
ap.add_argument("--out", default=str(HERE / "results-ollama.json"))
args = ap.parse_args()

questions = json.loads(pathlib.Path(args.questions).read_text(encoding="utf-8"))["questions"]
results = {"tool": "ollama", "model": args.model, "questions": []}


def ask(prompt):
    body = json.dumps({
        "model": args.model,
        "prompt": prompt,
        "stream": False,
        "think": False,
        "options": {"temperature": 0},
    }).encode()
    req = urllib.request.Request(args.host + "/api/generate", body,
                                 {"Content-Type": "application/json"})
    t0 = time.monotonic()
    with urllib.request.urlopen(req, timeout=600) as r:
        data = json.loads(r.read())
    return data.get("response", ""), time.monotonic() - t0


def norm(s):
    return re.sub(r"[,\s]", "", s.lower())


correct = refused = 0
answerable = sum(1 for q in questions if not q.get("unanswerable"))
for q in questions:
    text, elapsed = ask(q["q"])
    text = re.sub(r"<think>.*?</think>", "", text, flags=re.S).strip()
    row = {"id": q["id"], "q": q["q"], "seconds": round(elapsed, 1), "answer": text}
    if q.get("unanswerable"):
        # Without documents, a refusal means saying it does not know.
        row["correct"] = bool(re.search(r"(don't|do not|cannot|can't|no information|not have)", text, re.I))
        refused += bool(row["correct"])
    else:
        row["correct"] = any(norm(e) in norm(text) for e in q["expect"])
        correct += bool(row["correct"])
    results["questions"].append(row)
    print(f"  {q['id']:>2}. {'ok ' if row['correct'] else 'MISS'} {elapsed:5.1f}s  {text[:90]}")

results["score"] = {"answerable": answerable, "answered_correctly": correct,
                    "unanswerable": len(questions) - answerable, "refused_correctly": refused,
                    "median_seconds": round(sorted(r["seconds"] for r in results["questions"])[len(questions) // 2], 1)}
pathlib.Path(args.out).write_text(json.dumps(results, indent=2, ensure_ascii=False), encoding="utf-8")
s = results["score"]
print(f"\nanswers {s['answered_correctly']}/{answerable}, refusals {s['refused_correctly']}/{s['unanswerable']}, "
      f"median {s['median_seconds']}s -> {args.out}")
