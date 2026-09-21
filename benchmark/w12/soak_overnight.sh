#!/usr/bin/env bash
# SPDX-License-Identifier: BUSL-1.1
#
# soak_overnight.sh — start the 8-hour soak and leave it running.
#
# Keeps the Mac awake for the duration (caffeinate) and detaches, so the
# run survives the terminal being closed. Check the result in the
# morning with:
#
#     cat benchmark/w12/soak-latest.log
#
# Close other AI applications first: two model runtimes do not fit on a
# 16 GB Mac, and the soak will report failures that are really GPU
# contention.
set -eu

HOURS=${1:-8}
# The soak asks a running collection, which must have been built with the
# current default embedding model, or every request comes back as a model
# mismatch. The e5 data directory from the eval is the right one.
COLLECTION=${2:-bench}
HERE=$(cd "$(dirname "$0")" && pwd)
DATA=${3:-$HERE/../w0/data-e5}
LOG=$HERE/soak-$(date +%Y%m%d-%H%M%S).log

if pgrep -x ollama >/dev/null 2>&1 || pgrep -f "AnythingLLM" >/dev/null 2>&1; then
    echo "Another model runner is running. Quit it first, or the soak will"
    echo "record GPU contention as failures."
    exit 2
fi
if [ ! -d "$DATA" ]; then
    echo "No indexed collection at $DATA. Build one first, e.g. run the eval:"
    echo "  python3 benchmark/w0/run_lisa.py --data $DATA \\"
    echo "    --embed-model models/e5-small-v2-q8_0.gguf --chat-model models/Qwen3-4B-Q4_K_M.gguf"
    exit 2
fi

nohup caffeinate -i "$HERE/soak.sh" "$HOURS" "$COLLECTION" "$DATA" > "$LOG" 2>&1 &
ln -sf "$LOG" "$HERE/soak-latest.log"
echo "soak started, pid $!, $HOURS hours"
echo "log: $LOG"
echo "check in the morning:  tail -20 $HERE/soak-latest.log"
