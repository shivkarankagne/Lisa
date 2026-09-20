#!/usr/bin/env bash
# SPDX-License-Identifier: BUSL-1.1
#
# crash_serve.sh — W12 crash-recovery test for the running product.
#
# test_store_crash covers the storage layer. This covers the program: it
# kills `lisa serve` with SIGKILL partway through indexing a folder, then
# starts it again and checks that the collection opens, answers, and can
# finish indexing. A collection that needed a repair step by hand, or
# that lost documents indexed before the kill, fails.
#
# Usage: crash_serve.sh [trials] [corpus-dir]
set -u

TRIALS=${1:-5}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CORPUS=${2:-$ROOT/benchmark/w0/corpus/synthetic}
BIN=$ROOT/build/lisa
PORT=${PORT:-8732}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/lisa_crash.XXXXXX")
TOKEN=$(head -c 24 /dev/urandom | od -An -tx1 | tr -d ' \n')
SPID=

cleanup() { [ -n "$SPID" ] && kill -9 "$SPID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

[ -x "$BIN" ] || { echo "no binary at $BIN"; exit 2; }
[ -d "$CORPUS" ] || { echo "no corpus at $CORPUS"; exit 2; }

start_server() {
    "$BIN" serve --data "$WORK/data" --port "$PORT" --token "$TOKEN" >> "$WORK/server.log" 2>&1 &
    SPID=$!
    for _ in $(seq 60); do
        curl -fsS -m 5 "http://127.0.0.1:$PORT/v1/health" >/dev/null 2>&1 && return 0
        kill -0 "$SPID" 2>/dev/null || { echo "server exited at start"; return 1; }
        sleep 1
    done
    echo "server did not come up"
    return 1
}

api() {
    local method=$1 path=$2 body=${3:-}
    if [ -n "$body" ]; then
        curl -fsS -m 300 -X "$method" -H "Authorization: Bearer $TOKEN" \
             -H "Content-Type: application/json" -d "$body" "http://127.0.0.1:$PORT$path"
    else
        curl -fsS -m 300 -X "$method" -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:$PORT$path"
    fi
}

fail=0
for trial in $(seq "$TRIALS"); do
    echo "--- trial $trial"
    start_server || { fail=$((fail + 1)); continue; }

    # Ingest creates the collection; there is no separate create route.
    api POST "/v1/collections/crash/ingest" "{\"paths\":[\"$CORPUS\"]}" > "$WORK/job.json" 2>/dev/null

    # Kill somewhere inside the indexing run, not before it starts.
    sleep $(( (RANDOM % 8) + 3 ))
    before=$(api GET "/v1/collections/crash/documents" 2>/dev/null | python3 -c \
        "import json,sys; print(json.load(sys.stdin).get('readable', 0))" 2>/dev/null || echo 0)
    kill -9 "$SPID" 2>/dev/null
    wait "$SPID" 2>/dev/null
    SPID=
    echo "    killed after $before documents were readable"

    # It must come straight back up, with no repair step by hand.
    start_server || { echo "    FAIL: did not restart"; fail=$((fail + 1)); continue; }

    after=$(api GET "/v1/collections/crash/documents" 2>/dev/null | python3 -c \
        "import json,sys; print(json.load(sys.stdin).get('readable', 0))" 2>/dev/null || echo "ERR")
    if [ "$after" = "ERR" ]; then
        echo "    FAIL: collection unreadable after restart"
        fail=$((fail + 1))
    elif [ "$after" -lt "$before" ]; then
        echo "    FAIL: lost documents, $before before the kill, $after after"
        fail=$((fail + 1))
    else
        echo "    $after documents after restart"
    fi

    # Indexing the same folder again must finish and leave it usable.
    api POST "/v1/collections/crash/ingest" "{\"paths\":[\"$CORPUS\"]}" > /dev/null 2>&1
    for _ in $(seq 120); do
        state=$(api GET "/v1/jobs" 2>/dev/null | python3 -c \
            "import json,sys; j=json.load(sys.stdin).get('jobs',[]); print(j[0]['state'] if j else 'none')" \
            2>/dev/null || echo none)
        [ "$state" = "running" ] || [ "$state" = "queued" ] || break
        sleep 5
    done
    hits=$(api POST "/v1/collections/crash/search" '{"query":"pump bearing","topk":3}' 2>/dev/null | \
           python3 -c "import json,sys; print(len(json.load(sys.stdin).get('hits',[])))" 2>/dev/null || echo 0)
    [ "$hits" -gt 0 ] && echo "    search works after recovery ($hits hits)" || {
        echo "    FAIL: search returned nothing after recovery"; fail=$((fail + 1)); }

    kill -INT "$SPID" 2>/dev/null; wait "$SPID" 2>/dev/null; SPID=
    rm -rf "$WORK/data"
done

echo
if [ "$fail" -eq 0 ]; then echo "PASS: $TRIALS trials recovered"; else echo "FAIL: $fail of $TRIALS trials"; fi
exit $(( fail > 0 ))
