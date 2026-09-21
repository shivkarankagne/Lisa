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
| Clean-machine test | `manual-tests.md` section A | by hand |
| Offline test | `manual-tests.md` section B | by hand |
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

For the eight-hour run, use `soak_overnight.sh`: it refuses to start if
another model runner is up, keeps the Mac awake, and detaches so the run
survives the terminal closing.

    benchmark/w12/soak_overnight.sh          # then read soak-latest.log in the morning

## manual-tests.md

The clean-machine and offline tests, which need a fresh macOS account
and the network off. About 40 minutes for both, run in one sitting.
