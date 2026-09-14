#!/bin/bash
# test_cli.sh — end-to-end test of the LISA CLI
# Note: set -e is NOT used, because we deliberately run commands that
# return non-zero exit codes and check them.

BIN=~/lisa_assembly/build/lisa
IDX=~/lisa_assembly/vectors_768.bin
QRY=~/lisa_assembly/query_768.txt
DIM=768
TOPK=5

if [ ! -x "$BIN" ]; then
    echo "FAIL: binary not found: $BIN"
    exit 1
fi
if [ ! -f "$IDX" ]; then
    echo "FAIL: index not found: $IDX"
    exit 1
fi
if [ ! -f "$QRY" ]; then
    echo "FAIL: query not found: $QRY"
    exit 1
fi

PASS=0
FAIL=0

check() {
    local name="$1"
    local want_rc="$2"
    shift 2
    "$BIN" "$@" > /dev/null 2>&1
    local got_rc=$?
    if [ "$got_rc" -eq "$want_rc" ]; then
        echo "  PASS: $name (rc=$got_rc)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name (rc=$got_rc, want $want_rc)"
        FAIL=$((FAIL + 1))
    fi
    return 0
}

echo "CLI tests"

check "success" 0 --index "$IDX" --dim "$DIM" --topk "$TOPK" --query "$QRY"
check "missing index file" 2 --index /nonexistent --dim "$DIM" --query "$QRY"
check "dimension mismatch" 4 --index "$IDX" --dim 64 --query "$QRY"
check "missing query file" 3 --index "$IDX" --dim "$DIM" --query /nonexistent
check "missing arg" 1 --index "$IDX"

LINES=$("$BIN" --index "$IDX" --dim "$DIM" --topk "$TOPK" --query "$QRY" | wc -l | tr -d ' ')
if [ "$LINES" -eq "$TOPK" ]; then
    echo "  PASS: output line count ($LINES)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: output line count ($LINES, want $TOPK)"
    FAIL=$((FAIL + 1))
fi

echo ""
echo "PASS: $PASS"
echo "FAIL: $FAIL"

if [ "$FAIL" -eq 0 ]; then
    exit 0
else
    exit 1
fi
