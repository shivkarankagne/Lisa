#!/bin/bash
# SPDX-License-Identifier: BUSL-1.1
# test_gui.sh — the GUI in headless Chrome (tests/gui_check.mjs) against a
# running `lisa serve`. Exits 77 (reported as skipped) without Node or Chrome.
#
# Environment (set by CMake): LISA_BIN, LISA_MODELS, LISA_FIXTURES.

BIN=${LISA_BIN:?LISA_BIN not set}
HERE=$(cd "$(dirname "$0")" && pwd)
command -v node >/dev/null || { echo "SKIP: node not found"; exit 77; }
WORK=$(mktemp -d "${TMPDIR:-/tmp}/lisa_gui.XXXXXX")
trap 'kill -INT $SPID 2>/dev/null; wait $SPID 2>/dev/null; rm -rf "$WORK"' EXIT
mkdir -p "$WORK/docs" "$WORK/shots"
echo "Pump P-7 failed on Tuesday: the main bearing seized after weeks of rising vibration." > "$WORK/docs/pump.txt"
[ -n "${LISA_FIXTURES:-}" ] && cp "$LISA_FIXTURES/two_pages.pdf" "$WORK/docs/"

MODELS_FLAG=""
if [ -n "${LISA_MODELS:-}" ] && [ -f "$LISA_MODELS/Qwen3-4B-Q4_K_M.gguf" ] && \
   [ -f "$LISA_MODELS/Qwen3-Embedding-0.6B-Q8_0.gguf" ]; then
    "$BIN" model --data "$WORK/data" --set "$LISA_MODELS/Qwen3-4B-Q4_K_M.gguf" >/dev/null &&
    "$BIN" model --data "$WORK/data" --set "$LISA_MODELS/Qwen3-Embedding-0.6B-Q8_0.gguf" >/dev/null &&
    MODELS_FLAG="--models"
fi

"$BIN" serve --data "$WORK/data" --port 0 > "$WORK/serve.log" 2>&1 &
SPID=$!
for _ in $(seq 1 300); do grep -q "GUI:" "$WORK/serve.log" && break; sleep 0.1; done
URL=$(grep -o 'http://127.0.0.1:[0-9]*/#token=[0-9a-f]*' "$WORK/serve.log")
[ -n "$URL" ] || { echo "FAIL: server did not start: $(cat "$WORK/serve.log")"; exit 1; }

node "$HERE/gui_check.mjs" "$URL" "$WORK/docs" "${LISA_GUI_SHOTS:-$WORK/shots}" $MODELS_FLAG
