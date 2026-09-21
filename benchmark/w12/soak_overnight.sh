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
HERE=$(cd "$(dirname "$0")" && pwd)
LOG=$HERE/soak-$(date +%Y%m%d-%H%M%S).log

if pgrep -x ollama >/dev/null 2>&1 || pgrep -f "AnythingLLM" >/dev/null 2>&1; then
    echo "Another model runner is running. Quit it first, or the soak will"
    echo "record GPU contention as failures."
    exit 2
fi

nohup caffeinate -i "$HERE/soak.sh" "$HOURS" > "$LOG" 2>&1 &
ln -sf "$LOG" "$HERE/soak-latest.log"
echo "soak started, pid $!, $HOURS hours"
echo "log: $LOG"
echo "check in the morning:  tail -20 $HERE/soak-latest.log"
