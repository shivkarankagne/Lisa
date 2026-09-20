# W0 / W12 benchmark

Same documents, same questions, for LISA and for the tools people would
otherwise use. Nothing here runs automatically; it is measured by hand
on an idle machine, because model work saturates the GPU.

## The corpus (51 documents)

    benchmark/w0/fetch_corpus.sh      26 public-domain documents (IRS, CFR, Gutenberg)
    benchmark/w0/make_synthetic.py    25 business documents with known facts

The public half is verified against `corpus.sha256` on later runs, so
the benchmark stays comparable. The synthetic half is generated
deterministically and includes Word files, Hindi and Marathi text, and
two image-only PDFs that need text recognition.

## The questions

`questions.json`: 58 questions — 54 answerable, 4 that must be refused,
which meets the W12 exit criterion of at least 50. Ground truth was read
from the documents themselves; `expect` lists the strings a correct
answer contains, and `source` the document that should be cited.

`run_lisa.py` scores four things: whether the answer contains the fact,
whether it cites the right document, whether retrieval put that document
in the top 10 at all (recall@10), and whether an unanswerable question
is refused. Recall is scored separately so that a retrieval fault can be
told apart from a generation fault.

## Running

    benchmark/w0/fetch_corpus.sh
    benchmark/w0/make_synthetic.py
    benchmark/w0/run_lisa.py            # indexes, asks, scores; writes results-lisa.json
    benchmark/w0/run_ollama.py          # a local model with no documents, for contrast

Run them one at a time. Two 4-billion-parameter models do not fit on a
16 GB Mac at once: sharing the GPU makes indexing fail and answers come
back empty (report 013).

## What to record

Install time and download size, disk used by the index, peak memory,
indexing time, answers correct, citations correct, refusals correct, and
median time per answer.
