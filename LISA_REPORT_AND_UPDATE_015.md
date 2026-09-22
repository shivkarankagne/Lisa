# LISA — Report and Update 015

**Date:** 2026-09-21
**Type:** Work package report (in progress)
**Governing documents:** `LISA_COMPLETION_PLAN.md`, `CLAUDE.md`
**Work package:** W12 — Validation

---

## 1. Status

W12 is partly done. The eval set and the retrieval and answer scores are
measured; the crash and soak tests are written; three criteria need a
second machine or a person at the keyboard, and one is blocked.

| Acceptance criterion (plan §6 W12) | Status |
| :--- | :--- |
| Eval set ≥ 50 questions, recall@k and citation accuracy in a report | **Done** — 58 questions, scores below |
| Crash-recovery test | **Done** — 3 of 3 trials recovered, §6 |
| 8-hour soak on `serve` | **Done** — 8 h, 4,316 requests, 0 failures, memory -1.5%, §8 |
| Clean-machine test | **Open** — needs a fresh macOS user account |
| Offline test (Wi-Fi off from first copy to first answer) | **Open** |
| Matches or beats W0 | **Blocked** — the competitors were uninstalled to free disk |
| Binary size and `otool -L` recorded | **Done** — below |

## 2. The eval set

`benchmark/w0/questions.json`, 58 questions over the 51-document corpus
(26 public documents, 25 generated ones with known facts): 54 answerable
and 4 that must be refused. Ground truth was read from the documents.
The questions cover policies, an invoice, a bank statement, board
minutes, a vendor agreement, a quotation, a calibration certificate, an
insurance summary, an OCR-only scan, Hindi and Marathi text, US federal
regulations, IRS publications, and four novels.

## 3. Measured (2026-09-20, M2 16 GB, idle)

**Indexing:** 51 files, 11,259 chunks, 52 minutes, peak RSS 1,815 MB,
index 66.6 MB on disk for 35.1 MB of documents.

| Score | Result |
| :--- | :--- |
| Answers containing the fact | **50 / 54** |
| Citing the right document | **52 / 54** |
| File recall@10 | **54 / 54** |
| Passage recall@10 | **52 / 54** |
| Unanswerable refused | **4 / 4** |
| Median time per answer | **9.3 s** |

**Binary:** 15,532,504 bytes (14.8 MB). `otool -L` lists only macOS
system frameworks (Accelerate, Foundation, Metal, MetalKit, Vision,
AppKit, CoreFoundation, CoreGraphics, Security, WebKit, Cocoa) plus
`libSystem`, `libc++` and `libobjc`. The single-executable rule holds.

## 4. The four wrong answers

| # | Question | What happened |
| :--- | :--- | :--- |
| 19, 55 | "How does Pride and Prejudice begin?", "How does Moby-Dick begin?" | Retrieval returned the right novel but no passage holding the opening line, and LISA refused. The refusal is correct behaviour; the retrieval is the fault. A positional question ("how does it begin") shares no words with "It is a truth universally acknowledged", so neither the keyword nor the vector side has anything to match |
| 16 | "When may a motor vehicle not be operated on a tire?" | Answered with a true rule from the document (cold inflation pressure below the load minimum) that was not the one the question expected. Scored wrong; arguably a narrow ground truth rather than a wrong answer |
| 18 | "What percentage … child and dependent care credit … dollar limit for one qualifying person?" | **A real defect.** Retrieval was correct: a passage says "$3,000 (one qualifying person)". LISA answered "20% … $5,000". $5,000 appears in the passages, but as the dependent-care-benefits exclusion, a different figure; 20% appears nowhere at all |

## 5. The finding that matters

Question 18 is the same failure as the one fixed the day before in
report 014 §2.11: the model states a number that no passage supports and
cites a document anyway. That fix was a prompt rule, and question 18
shows a prompt rule does not close the hole — it lowers the rate.

Retrieval is not the weak part. File recall is 54/54, passage recall
52/54, and the two passage misses are exactly the two questions LISA
declined. Every wrong answer but one happened with the right passage in
front of the model.

The structural fix is citation verification: check that the facts in an
answer appear in the passage it cites, and refuse or drop the sentence
when they do not. The seam for it already exists — citations carry the
document ID, offsets and a content hash (plan §7, built in W2 and W8) —
but the work itself is plan §8, which `CLAUDE.md` puts out of scope
until W12 passes. Recorded here, not started.

## 6. Crash recovery

`benchmark/w12/crash_serve.sh` indexes a small folder and lets it
finish, starts a long indexing job over the whole corpus, waits until
that job is running, kills `lisa serve` with SIGKILL, and starts it
again. It then checks that the collection opens with no repair by hand,
has lost none of the documents committed before the kill, still indexes,
and still answers a search. `test_store_crash` covers the same ground
one layer down, in the storage engine.

**Result (2026-09-21): 3 of 3 trials recovered.** Each trial killed the
server with 25 documents committed and a job in flight; all 25 were
present after the restart, and search and indexing both worked.

The first version of this test was worthless and is worth recording as a
lesson: it slept a few seconds and then killed the server, but the small
folder finishes indexing in about two seconds, so every trial killed an
idle server and passed. The second version killed while a job ran but
before any document had been committed, because the first file of the
full corpus is a novel that takes minutes. Only the third version — index
something first, then interrupt a running job — tests what the criterion
is about, which is not losing committed work.

## 7. Not done

- Clean-machine and offline tests need a fresh macOS account and a
  machine with the Wi-Fi off.
- Clean-machine and offline tests still need a fresh macOS account.
- The W0 comparison needs AnythingLLM and Ollama installed again; they
  were removed at the user's request to free disk.
- Indexing 51 documents takes 52 minutes. That is the embedding model on
  the GPU, unchanged since report 014 §5.

## 8. Soak result (2026-09-22, M2 16 GB)

`benchmark/w12/soak.sh` ran `lisa serve` for 8 hours against the e5-built
benchmark collection, asking and searching in a loop and sampling the
server's resident memory every minute.

| | |
| :--- | :--- |
| Duration | 8 hours |
| Requests | 4,316 |
| Failures | 0 |
| Memory, first eighth → last eighth | 3,107 MB → 3,059 MB (**-1.5%**, allowed +10%) |
| Verdict | **PASS** |

Memory did not grow; it fell slightly. No leak, no crash, no failed
request under continuous load.
