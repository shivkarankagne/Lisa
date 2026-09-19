#!/bin/bash
# SPDX-License-Identifier: BUSL-1.1
# test_cli.sh — end-to-end test of the lisa command line (W9).
#
# Environment (set by CMake):
#   LISA_BIN              the lisa binary
#   LISA_BUILD_COLLECTION helper that writes a LISA 0.1 (v1) collection
#   LISA_TEST_INDEX       a v1 index file for it
#   LISA_MODELS           directory with the default models (tests that
#                         need them are skipped if they are missing)
#   LISA_FIXTURES         tests/fixtures
# set -e is not used: commands are expected to fail and are checked.

BIN=${LISA_BIN:?LISA_BIN not set}
MODELS=${LISA_MODELS:-}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/lisa_cli.XXXXXX")
DATA="$WORK/data"
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0
ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad()  { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# rc <name> <want_rc> <cmd...>: run, compare the exit code, keep output in $OUT.
rc() {
    local name="$1" want="$2"
    shift 2
    OUT=$("$@" 2>&1)
    local got=$?
    if [ "$got" -eq "$want" ]; then ok "$name (rc=$got)"; else bad "$name (rc=$got, want $want): $OUT"; fi
}
has() { if printf '%s' "$OUT" | grep -q -- "$2"; then ok "$1"; else bad "$1: output lacks '$2': $OUT"; fi; }

echo "lisa CLI tests"

# ---- no models needed ------------------------------------------------------
rc "--version" 0 "$BIN" --version
has "--version prints the version" "^lisa [0-9]*\.[0-9]*\.[0-9]*$"
rc "help" 0 "$BIN" --help
has "help lists commands" "lisa ask"
rc "no arguments" 1 "$BIN"
rc "unknown command" 1 "$BIN" frobnicate
rc "unknown option" 1 "$BIN" search --data "$DATA" --bogus
rc "missing --collection" 1 "$BIN" search --data "$DATA" --query x
rc "invalid collection name" 1 "$BIN" search --data "$DATA" --collection "../etc" --query x
rc "bad --topk" 1 "$BIN" search --data "$DATA" --collection c --query x --topk 0
rc "ask without a question" 1 "$BIN" ask --data "$DATA" --collection c
rc "ingest without paths" 1 "$BIN" ingest --data "$DATA" --collection c
rc "ingest missing path" 2 "$BIN" ingest --data "$DATA" --collection c "$WORK/nope"
# `lisa gui` opens a window, so it is tested by test_gui (headless Chrome), not here.
[ -d "$DATA/collections" ] && ok "data dir and collections/ created" || bad "data dir not created"

# migrate a LISA 0.1 collection
if [ -x "${LISA_BUILD_COLLECTION:-}" ] && [ -f "${LISA_TEST_INDEX:-}" ]; then
    "$LISA_BUILD_COLLECTION" "$LISA_TEST_INDEX" "$WORK/v1" >/dev/null 2>&1
    rc "migrate v1" 0 "$BIN" migrate --from "$WORK/v1" --to "$WORK/v2" --model legacy-768
    [ -f "$WORK/v2/meta.sqlite" ] && ok "migrated collection written" || bad "no meta.sqlite after migrate"
    rc "migrate onto existing" 5 "$BIN" migrate --from "$WORK/v1" --to "$WORK/v2"
    rc "migrate missing source" 2 "$BIN" migrate --from "$WORK/none" --to "$WORK/v3"
    rc "migrate without --to" 1 "$BIN" migrate --from "$WORK/v1"
else
    bad "migrate: helper or index missing"
fi

# serve: starts, answers /v1/health, stops cleanly on Ctrl-C (SIGINT)
"$BIN" serve --data "$DATA" --port 0 --token cli-test-token-0123456789 > "$WORK/serve.log" 2>&1 &
SPID=$!
for _ in $(seq 1 300); do grep -q "serving" "$WORK/serve.log" && break; sleep 0.1; done
PORT=$(grep -o '127.0.0.1:[0-9]*' "$WORK/serve.log" | head -1 | cut -d: -f2)
if [ -n "$PORT" ]; then
    OUT=$(curl -s "http://127.0.0.1:$PORT/v1/health")
    has "serve: /v1/health" '"status":"ok"'
    OUT=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/v1/collections")
    [ "$OUT" = "401" ] && ok "serve: token required" || bad "serve: /v1/collections without token gave $OUT"
else
    bad "serve did not start: $(cat "$WORK/serve.log")"
fi
kill -INT "$SPID" 2>/dev/null
wait "$SPID"
SRC=$?
[ "$SRC" -eq 0 ] && ok "serve stops cleanly on SIGINT" || bad "serve exit code $SRC"

# ---- with models -------------------------------------------------------------
if [ -n "$MODELS" ] && [ -f "$MODELS/Qwen3-4B-Q4_K_M.gguf" ] && [ -f "$MODELS/Qwen3-Embedding-0.6B-Q8_0.gguf" ]; then
    rc "model --set chat" 0 "$BIN" model --data "$DATA" --set "$MODELS/Qwen3-4B-Q4_K_M.gguf"
    has "chat model recognised" "Set chat model"
    rc "model --set embedding" 0 "$BIN" model --data "$DATA" --set "$MODELS/Qwen3-Embedding-0.6B-Q8_0.gguf"
    rc "model --set missing file" 2 "$BIN" model --data "$DATA" --set "$WORK/none.gguf"
    rc "model shows both" 0 "$BIN" model --data "$DATA"
    has "model verified" "verified: qwen3-4b-q4_k_m"
    rc "model --json" 0 "$BIN" model --data "$DATA" --json
    has "model --json status" '"status": "verified"'

    mkdir -p "$WORK/docs"
    echo "Pump P-7 failed on Tuesday: the main bearing seized after weeks of rising vibration." > "$WORK/docs/pump.txt"
    echo "Employees get 24 days of paid annual leave per year." > "$WORK/docs/leave.md"
    [ -n "${LISA_FIXTURES:-}" ] && cp "$LISA_FIXTURES/two_pages.pdf" "$WORK/docs/"
    rc "ingest" 0 "$BIN" ingest --data "$DATA" --collection plant "$WORK/docs"
    has "ingest summary" "Indexed 'plant': 3 added"
    rc "ingest again (unchanged)" 0 "$BIN" ingest --data "$DATA" --collection plant "$WORK/docs"
    has "nothing re-indexed" "3 unchanged"
    rc "ingest --json" 0 "$BIN" ingest --data "$DATA" --collection plant "$WORK/docs" --json
    has "ingest --json counters" '"files_unchanged": 3'

    rc "search" 0 "$BIN" search --data "$DATA" --collection plant --query "annual leave days" --topk 2
    has "search finds the leave note" "leave.md"
    rc "search --json" 0 "$BIN" search --data "$DATA" --collection plant --query "annual leave" --json
    has "search --json hits" '"hits"'
    rc "search unknown collection" 2 "$BIN" search --data "$DATA" --collection nothing --query x

    rc "ask" 0 "$BIN" ask --data "$DATA" --collection plant "Why did pump P-7 fail?"
    has "ask answers" "bearing"
    has "ask lists sources" "Sources:"
    rc "ask --json" 0 "$BIN" ask --data "$DATA" --collection plant --json "How often must pump bearings be inspected?"
    has "ask --json cites the PDF page" '"page": 1'
    has "ask --json found" '"found": true'
    rc "ask off-topic" 0 "$BIN" ask --data "$DATA" --collection plant "Who won the cricket world cup in 2011?"
    has "ask says not found" "I could not find this in your documents."
else
    echo "  SKIP: model tests (models not present)"
fi

echo "lisa CLI: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
