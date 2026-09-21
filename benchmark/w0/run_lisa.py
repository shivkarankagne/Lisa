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
ap.add_argument("--embed-model", help="embedding .gguf to use (default: whatever models/ offers)")
ap.add_argument("--chat-model", help="generation .gguf to use")
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
    """
    The data directory is wiped above, which takes config.json with it.
    Models chosen for this run are therefore set afterwards, not before,
    or `lisa` falls back to whatever it finds in models/ — which silently
    ran the wrong embedding model and made a whole comparison worthless.
    """
    for m in (args.embed_model, args.chat_model):
        if m:
            q = run([args.bin, "model", "--data", args.data, "--set", m])[0]
            if q.returncode != 0:
                sys.exit(f"could not set model {m}: {q.stderr[-500:]}")
    print("indexing...", flush=True)
    p, elapsed, peak = run([args.bin, "ingest", "--data", args.data,
                            "--collection", args.collection, args.corpus, "--json"])
    if p.returncode != 0:
        sys.exit(f"ingest failed: {p.stderr[-2000:]}")
    st = json.loads(p.stdout)
    results["index"] = {"seconds": round(elapsed, 1), "peak_rss_mb": round(peak, 1), **st}
    # Record what actually ran, so two results files can be compared safely.
    mp = run([args.bin, "model", "--data", args.data, "--json"])[0]
    if mp.returncode == 0:
        try:
            results["models"] = json.loads(mp.stdout)
        except ValueError:
            pass
    disk = sum(f.stat().st_size for f in pathlib.Path(args.data).rglob("*") if f.is_file())
    results["index"]["index_bytes"] = disk
    print(f"  {st['files_added']} files, {st['chunks_added']} chunks, {elapsed:.1f}s, "
          f"peak {peak:.0f} MB, index {disk / 1e6:.1f} MB")

# ---- ask -----------------------------------------------------------------------
def contains_fact(text, expected):
    """
    Does `text` state `expected`? Commas and spaces are ignored, because
    documents write 3,000 and an answer may write 3000. A number must not
    match inside a longer one: "35" is not present in "1,350", and
    scoring it as present overstates both recall and accuracy.
    """
    t = re.sub(r"[,\s]", "", text.lower())
    x = re.sub(r"[,\s]", "", expected.lower())
    if not x:
        return False
    i = t.find(x)
    while i >= 0:
        before = t[i - 1] if i > 0 else ""
        after = t[i + len(x)] if i + len(x) < len(t) else ""
        if not (x[0].isdigit() and before.isdigit()) and not (x[-1].isdigit() and after.isdigit()):
            return True
        i = t.find(x, i + 1)
    return False


ok_answer = ok_citation = ok_refusal = 0
ok_recall_file = ok_recall_passage = 0
RECALL_K = 10
answerable = sum(1 for q in questions if not q.get("unanswerable"))
unanswerable = len(questions) - answerable

for q in questions:
    """
    Retrieval is scored separately from the answer, and at two levels.
    File recall asks whether the right document appears at all; passage
    recall asks whether a retrieved passage actually contains the fact.
    Only the second one means the model could have answered: a 700 KB
    novel is one file, so having it in the top ten says nothing.
    """
    recalled = recalled_passage = None
    if not q.get("unanswerable") and q.get("source"):
        ps, _, _ = run([args.bin, "search", "--data", args.data, "--collection", args.collection,
                        "--json", "--topk", str(RECALL_K), "--query", q["q"]])
        hits = json.loads(ps.stdout)["hits"] if ps.returncode == 0 else []
        recalled = q["source"] in [os.path.basename(h["path"]) for h in hits]
        recalled_passage = any(contains_fact(h.get("text", ""), e)
                               for h in hits for e in q["expect"])
        ok_recall_file += bool(recalled)
        ok_recall_passage += bool(recalled_passage)

    p, elapsed, _ = run([args.bin, "ask", "--data", args.data, "--collection", args.collection,
                         "--json", q["q"]])
    row = {"id": q["id"], "q": q["q"], "seconds": round(elapsed, 1),
           "recalled_file": recalled, "recalled_passage": recalled_passage}
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
        row["correct"] = any(contains_fact(text, e) for e in q["expect"])
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
    "file_recall_at_%d" % RECALL_K: ok_recall_file,
    "passage_recall_at_%d" % RECALL_K: ok_recall_passage,
    "unanswerable": unanswerable,
    "refused_correctly": ok_refusal,
    "median_seconds": round(sorted(r["seconds"] for r in results["questions"])[len(questions) // 2], 1),
}
pathlib.Path(args.out).write_text(json.dumps(results, indent=2, ensure_ascii=False), encoding="utf-8")
s = results["score"]
print(f"\nanswers {s['answered_correctly']}/{answerable}, citations {s['cited_correct_source']}/{answerable}, "
      f"file recall@{RECALL_K} {s['file_recall_at_%d' % RECALL_K]}/{answerable}, "
      f"passage recall@{RECALL_K} {s['passage_recall_at_%d' % RECALL_K]}/{answerable}, "
      f"refusals {s['refused_correctly']}/{unanswerable}, median {s['median_seconds']}s")
print(f"written to {args.out}")
