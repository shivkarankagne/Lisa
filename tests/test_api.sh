#!/bin/bash
# test_api.sh — end-to-end test of the LISA HTTP API
# Note: set -e is NOT used, because we expect non-zero exit codes.

BIN=~/lisa_assembly/build/lisa
PORT=18080
COLL=/tmp/lisa_api_test_coll
QRY=/tmp/lisa_api_test_query.bin

if [ ! -x "$BIN" ]; then
    echo "FAIL: binary not found: $BIN"
    exit 1
fi

PASS=0
FAIL=0

check() {
    local name="$1"
    local cond="$2"
    if [ "$cond" = "1" ]; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name"
        FAIL=$((FAIL + 1))
    fi
    return 0
}

echo "HTTP API tests"

# prepare test collection (16 vectors, dim 4, values 0..15)
python3 - << 'ENDOFPY'
import struct, os, shutil
d = "/tmp/lisa_api_test_coll"
shutil.rmtree(d, ignore_errors=True)
os.makedirs(d, exist_ok=True)
n, dim = 16, 4
with open(d + "/header.bin", "wb") as f:
    f.write(b"LISA")
    f.write(struct.pack("<I", 2))
    f.write(struct.pack("<I", n))
    f.write(struct.pack("<I", dim))
    f.write(struct.pack("<I", n))
with open(d + "/vectors.bin", "wb") as f:
    for i in range(n):
        for j in range(dim):
            f.write(struct.pack("<f", float(i)))
with open("/tmp/lisa_api_test_query.bin", "wb") as f:
    for _ in range(dim):
        f.write(struct.pack("<f", 3.5))
ENDOFPY

if [ ! -d "$COLL" ]; then
    echo "FAIL: could not prepare test collection"
    exit 1
fi

"$BIN" --serve --port "$PORT" &
SERVER_PID=$!
sleep 1

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "FAIL: server did not start"
    exit 1
fi

CODE=$(curl -s -o /tmp/api_health_body.txt -w "%{http_code}" "http://127.0.0.1:$PORT/health")
check "/health returns 200" "$([ "$CODE" = "200" ] && echo 1 || echo 0)"
BODY=$(cat /tmp/api_health_body.txt)
check "/health body is ok" "$([ "$BODY" = "ok" ] && echo 1 || echo 0)"

CODE=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$PORT/nope")
check "unknown path returns 404" "$([ "$CODE" = "404" ] && echo 1 || echo 0)"

CODE=$(curl -s -o /tmp/api_search_body.txt -w "%{http_code}" \
    --data-binary @"$QRY" \
    -H "Content-Type: application/octet-stream" \
    "http://127.0.0.1:$PORT/search?collection=$COLL&topk=3")
check "/search returns 200" "$([ "$CODE" = "200" ] && echo 1 || echo 0)"
LINES=$(wc -l < /tmp/api_search_body.txt | tr -d ' ')
check "/search returns 3 lines" "$([ "$LINES" = "3" ] && echo 1 || echo 0)"

FIRST=$(head -1 /tmp/api_search_body.txt | awk '{print $1}')
check "/search first is 3 or 4" "$([ "$FIRST" = "3" ] || [ "$FIRST" = "4" ] && echo 1 || echo 0)"

CODE=$(curl -s -o /dev/null -w "%{http_code}" \
    --data-binary @"$QRY" \
    -H "Content-Type: application/octet-stream" \
    "http://127.0.0.1:$PORT/search?collection=/nonexistent&topk=3")
check "/search missing collection returns 404" "$([ "$CODE" = "404" ] && echo 1 || echo 0)"

CODE=$(curl -s -o /dev/null -w "%{http_code}" \
    --data-binary "abc" \
    -H "Content-Type: application/octet-stream" \
    "http://127.0.0.1:$PORT/search?collection=$COLL&topk=3")
check "/search wrong body size returns 400" "$([ "$CODE" = "400" ] && echo 1 || echo 0)"

CODE=$(curl -s -o /dev/null -w "%{http_code}" \
    --data-binary @"$QRY" \
    -H "Content-Type: application/octet-stream" \
    "http://127.0.0.1:$PORT/search?topk=3")
check "/search missing collection param returns 400" "$([ "$CODE" = "400" ] && echo 1 || echo 0)"

kill "$SERVER_PID" 2>/dev/null
sleep 1
if kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "  WARN: server did not stop on SIGTERM, killing"
    kill -9 "$SERVER_PID" 2>/dev/null
fi
wait "$SERVER_PID" 2>/dev/null

echo ""
echo "PASS: $PASS"
echo "FAIL: $FAIL"

if [ "$FAIL" -eq 0 ]; then
    exit 0
else
    exit 1
fi
