# W12 — validation

The 1.0 exit criteria from the plan, §6 W12. The eval set lives with the
W0 benchmark (`benchmark/w0/questions.json`, 58 questions) because both
use the same corpus; the tests here are the ones W12 adds.

Nothing runs automatically. Model work saturates the GPU, so each of
these is run on an idle machine, one at a time.

| Criterion | How it is checked | State |
| :--- | :--- | :--- |
| Eval set ≥ 50 questions, recall@k and citation accuracy | `benchmark/w0/run_lisa.py` | 58 questions |
| Crash recovery | `crash_serve.sh` | script below |
| 8-hour soak on `serve` | `soak.sh` | script below |
| Clean-machine test | by hand: fresh macOS user account, copy binary + model, run the quickstart through the GUI and the CLI | by hand |
| Offline test | by hand: Wi-Fi off from first copy to first answer | by hand |
| Binary size and `otool -L` | `ls -l build/lisa`, `otool -L build/lisa` | recorded in the report |
| Matches or beats W0 | needs the competitors installed again | blocked |

## crash_serve.sh

    benchmark/w12/crash_serve.sh [trials] [corpus-dir]

Kills `lisa serve` with SIGKILL partway through indexing, then starts it
again and checks the collection opens with no repair by hand, has lost
none of the documents indexed before the kill, finishes indexing, and
answers a search. `test_store_crash` covers the same ground one layer
down, at the storage engine.

## soak.sh

    benchmark/w12/soak.sh [hours] [collection] [data-dir]

Asks and searches in a loop for eight hours by default, sampling the
server's resident memory every minute into `soak-<timestamp>/memory.csv`.
It fails if any request fails, or if the median memory of the last eighth
of the run is more than 10% above the first eighth — the shape a leak
takes over a long run. Needs a collection that is already indexed; the
W0 data directory is the default.
