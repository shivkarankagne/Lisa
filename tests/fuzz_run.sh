#!/bin/bash
# fuzz_run.sh — reproducible fuzz harness
#
# Usage: ./fuzz_run.sh <iterations> <base_seed>
#
# For each iteration:
#   - derive n, dim, k from (base_seed + i)
#   - run fuzz_gen
#   - compare scalar and asm outputs:
#       * indices must match exactly
#       * distances must match within abs=1e-4 OR rel=1e-4
#   - count pass/fail
#
# Comparison rule (documented in tests/README.md):
#   Two results are equivalent if the top-k index sequences are identical
#   AND for every position, |d_s - d_a| <= max(1e-4, 1e-4 * |d_s|).

set -e

ITERATIONS=${1:-200}
BASE_SEED=${2:-12345}

GEN=~/lisa_assembly/tests/fuzz_gen
CMP=~/lisa_assembly/tests/fuzz_compare.py
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

if [ ! -x "$GEN" ]; then
    echo "error: $GEN not found or not executable" >&2
    exit 2
fi
if [ ! -f "$CMP" ]; then
    echo "error: $CMP not found" >&2
    exit 2
fi

PASS=0
FAIL=0
FAIL_DETAILS=""

for i in $(seq 0 $((ITERATIONS - 1))); do
    SEED=$((BASE_SEED + i))
    N=$(( (SEED * 7) % 2000 + 1 ))
    DIM=$(( (SEED * 13) % 512 + 1 ))
    K=$(( (SEED * 17) % 50 + 1 ))

    OUT="$TMP/case.txt"
    if ! "$GEN" "$N" "$DIM" "$K" "$SEED" "$OUT" 2>/dev/null; then
        echo "gen failed: n=$N dim=$DIM k=$K seed=$SEED"
        FAIL=$((FAIL + 1))
        if [ $FAIL -ge 5 ]; then break; fi
        continue
    fi

    if python3 "$CMP" "$OUT" > "$TMP/cmp.log" 2>&1; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        FAIL_DETAILS="$FAIL_DETAILS
case n=$N dim=$DIM k=$K seed=$SEED
$(cat "$TMP/cmp.log")"
        if [ $FAIL -ge 5 ]; then
            echo "stopping after 5 failures"
            break
        fi
    fi
done

echo ""
echo "PASS: $PASS"
echo "FAIL: $FAIL"

if [ -n "$FAIL_DETAILS" ]; then
    echo ""
    echo "Failure details:"
    echo "$FAIL_DETAILS"
fi

if [ $FAIL -eq 0 ]; then
    exit 0
else
    exit 1
fi
