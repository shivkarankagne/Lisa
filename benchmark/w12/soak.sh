#!/usr/bin/env bash
# SPDX-License-Identifier: BUSL-1.1
#
# soak.sh — W12 soak test: `lisa serve` under continuous load.
#
# Asks and searches in a loop for a set number of hours, sampling the
# server's resident memory every minute. It fails if memory grows by more
# than the allowed share between the first hour and the last, which is
# what a leak looks like over a long run, or if the server stops
# answering.
#
# Usage: soak.sh [hours] [collection] [data-dir]
set -u

HOURS=${1:-8}
COLLECTION=${2:-bench}
DATA=${3:-$(cd "$(dirname "$0")/.." && pwd)/w0/data-lisa}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=$ROOT/build/lisa
PORT=${PORT:-8731}
OUT=$(cd "$(dirname "$0")" && pwd)/soak-$(date +%Y%m%d-%H%M%S)
GROWTH_ALLOWED_PERCENT=${GROWTH_ALLOWED_PERCENT:-10}

mkdir -p "$OUT"
[ -x "$BIN" ] || { echo "no binary at $BIN"; exit 2; }
[ -d "$DATA" ] || { echo "no data directory at $DATA (run the W0 benchmark first)"; exit 2; }

TOKEN=$(head -c 24 /dev/urandom | od -An -tx1 | tr -d ' \n')
"$BIN" serve --data "$DATA" --port "$PORT" --token "$TOKEN" > "$OUT/server.log" 2>&1 &
SPID=$!
trap 'kill -INT $SPID 2>/dev/null; wait $SPID 2>/dev/null' EXIT

for _ in $(seq 60); do
    curl -fsS -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:$PORT/v1/health" >/dev/null 2>&1 && break
    sleep 1
done

ask() {
    curl -fsS -m 300 -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
         -d "{\"messages\":[{\"role\":\"user\",\"content\":\"$1\"}]}" \
         "http://127.0.0.1:$PORT/v1/collections/$COLLECTION/ask"
}
search() {
    curl -fsS -m 60 -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
         -d "{\"query\":\"$1\",\"topk\":5}" \
         "http://127.0.0.1:$PORT/v1/collections/$COLLECTION/search"
}

QUESTIONS=("What is the monthly rent for the Yenda premises?"
           "How long is the warranty on the Snowdrop reader?"
           "What is the vibration limit for pump P-7?"
           "What is the sum insured per family?"
           "Who won the cricket world cup in 2011?")

END=$(( $(date +%s) + HOURS * 3600 ))
NEXT_SAMPLE=0
i=0
failures=0
echo "epoch,rss_kb,requests,failures" > "$OUT/memory.csv"

while [ "$(date +%s)" -lt "$END" ]; do
    q=${QUESTIONS[$(( i % ${#QUESTIONS[@]} ))]}
    ask "$q" > "$OUT/last-answer.json" 2>>"$OUT/errors.log" || failures=$((failures + 1))
    search "$q" > /dev/null 2>>"$OUT/errors.log" || failures=$((failures + 1))
    i=$((i + 1))

    now=$(date +%s)
    if [ "$now" -ge "$NEXT_SAMPLE" ]; then
        rss=$(ps -o rss= -p $SPID | tr -d ' ')
        [ -n "$rss" ] || { echo "server died after $i requests"; exit 1; }
        echo "$now,$rss,$i,$failures" >> "$OUT/memory.csv"
        NEXT_SAMPLE=$((now + 60))
    fi
done

python3 - "$OUT/memory.csv" "$GROWTH_ALLOWED_PERCENT" <<'PY'
import csv, statistics, sys
rows = list(csv.DictReader(open(sys.argv[1])))
allowed = float(sys.argv[2])
if len(rows) < 10:
    sys.exit("too few samples: %d" % len(rows))
rss = [int(r["rss_kb"]) for r in rows]
head = statistics.median(rss[: max(1, len(rss) // 8)])
tail = statistics.median(rss[-max(1, len(rss) // 8):])
growth = (tail - head) / head * 100.0
print("samples %d, requests %s, failures %s" % (len(rows), rows[-1]["requests"], rows[-1]["failures"]))
print("rss first eighth %.0f MB, last eighth %.0f MB, growth %+.1f%% (allowed %.0f%%)"
      % (head / 1024, tail / 1024, growth, allowed))
if int(rows[-1]["failures"]) > 0:
    sys.exit("FAIL: %s requests failed" % rows[-1]["failures"])
if growth > allowed:
    sys.exit("FAIL: memory grew %.1f%%" % growth)
print("PASS")
PY
