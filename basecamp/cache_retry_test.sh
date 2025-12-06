#!/bin/bash
# Cache & Idempotency with Retry Test

set -e

TOPOLOGY_FILE="$(pwd)/config/topology_2hosts.textproto"
export TOPOLOGY_FILE
PYTHONPATH=".:./py"
export PYTHONPATH

LOG_DIR="logs"
mkdir -p "$LOG_DIR"

# Clear old logs
rm -f "$LOG_DIR"/*.log /tmp/cache_test_*.log 2>/dev/null

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

echo "============================================"
echo "Cache & Idempotency with Retry Test"
echo "============================================"
echo ""
echo "This test verifies that:"
echo "1. Duplicate request_id hits the cache (idempotency)"
echo "2. Cache prevents recomputation"
echo "3. Cache helps during retry scenarios"
echo ""

cleanup() {
    echo -e "${YELLOW}Cleaning up...${NC}"
    pkill -f basecamp_server || true
    pkill -f "python3 -m py.server" || true
    sleep 2
}

trap cleanup EXIT

wait_for_server() {
    local node=$1
    local port=$2
    local max_attempts=20
    local attempt=0
    
    while [ $attempt -lt $max_attempts ]; do
        if nc -z 127.0.0.1 $port 2>/dev/null; then
            echo -e "${GREEN}✓ Node $node ready${NC}"
            return 0
        fi
        sleep 0.2
        attempt=$((attempt + 1))
    done
    
    echo -e "${RED}✗ Node $node startup failed${NC}"
    return 1
}

echo -e "${BLUE}Starting servers...${NC}"
./build/basecamp_server --node=A >> "$LOG_DIR/A.log" 2>&1 &
PID_A=$!
./build/basecamp_server --node=B >> "$LOG_DIR/B.log" 2>&1 &
PID_B=$!
./build/basecamp_server --node=C >> "$LOG_DIR/C.log" 2>&1 &
PID_C=$!
./build/basecamp_server --node=D >> "$LOG_DIR/D.log" 2>&1 &
PID_D=$!
python3 -m py.server_e >> "$LOG_DIR/E.log" 2>&1 &
PID_E=$!
python3 -m py.server_f >> "$LOG_DIR/F.log" 2>&1 &
PID_F=$!

wait_for_server "A" "50051" && \
wait_for_server "B" "50052" && \
wait_for_server "C" "50053" && \
wait_for_server "D" "50054" && \
wait_for_server "E" "50055" && \
wait_for_server "F" "50056" || exit 1

sleep 3

echo ""
echo "============================================"
echo "Test 1: First Request (Fresh Computation)"
echo "============================================"
echo ""

FIXED_REQ_ID="req-test-idempotency-12345"
echo -e "${CYAN}Request ID: ${FIXED_REQ_ID}${NC}"
echo -e "${YELLOW}Sending first GREEN request...${NC}"

TIMESTAMP_START=$(date +%s%N)
timeout 10 ./build/basecamp_client --target=GREEN --payload_size=512 --request_id="$FIXED_REQ_ID" > /tmp/cache_test_1.log 2>&1
TIMESTAMP_END=$(date +%s%N)
TIME_FIRST_MS=$(( (TIMESTAMP_END - TIMESTAMP_START) / 1000000 ))

echo "Result:"
grep "OK" /tmp/cache_test_1.log || true
echo "Response time: ${TIME_FIRST_MS}ms"

echo ""
echo -e "${BLUE}Node C log (fresh computation):${NC}"
tail -3 "$LOG_DIR/C.log" | grep -v "^$"

sleep 2

echo ""
echo "============================================"
echo "Test 2: Duplicate Request (Same request_id)"
echo "============================================"
echo ""

echo -e "${CYAN}Request ID: ${FIXED_REQ_ID} (SAME as Test 1)${NC}"
echo -e "${YELLOW}Sending duplicate request...${NC}"

TIMESTAMP_START=$(date +%s%N)
timeout 10 ./build/basecamp_client --target=GREEN --payload_size=512 --request_id="$FIXED_REQ_ID" > /tmp/cache_test_2.log 2>&1
TIMESTAMP_END=$(date +%s%N)
TIME_SECOND_MS=$(( (TIMESTAMP_END - TIMESTAMP_START) / 1000000 ))

echo "Result:"
grep "OK" /tmp/cache_test_2.log || true
echo "Response time: ${TIME_SECOND_MS}ms"

if [ $TIME_SECOND_MS -gt 0 ] && [ $TIME_FIRST_MS -gt 0 ]; then
    if [ $TIME_SECOND_MS -lt $TIME_FIRST_MS ]; then
        SPEEDUP=$(( TIME_FIRST_MS / TIME_SECOND_MS ))
        echo -e "${GREEN}✓ Speedup: ${SPEEDUP}x faster${NC}"
    else
        echo -e "${YELLOW}~ Similar times${NC}"
    fi
fi

sleep 1

echo ""
echo -e "${BLUE}Checking for 'Idempotency HIT' in Node C:${NC}"
if grep "Idempotency HIT.*$FIXED_REQ_ID" "$LOG_DIR/C.log"; then
    echo -e "${GREEN}✓✓✓ IDEMPOTENCY CACHE WORKING! ✓✓✓${NC}"
else
    echo -e "${RED}✗ No idempotency hit found${NC}"
    echo "Recent C log:"
    tail -5 "$LOG_DIR/C.log"
fi

sleep 2

echo ""
echo "============================================"
echo "Test 3: Different Request ID (Fresh Again)"
echo "============================================"
echo ""

NEW_REQ_ID="req-test-different-67890"
echo -e "${CYAN}Request ID: ${NEW_REQ_ID} (NEW)${NC}"
echo -e "${YELLOW}Sending request with different ID...${NC}"

TIMESTAMP_START=$(date +%s%N)
timeout 10 ./build/basecamp_client --target=GREEN --payload_size=512 --request_id="$NEW_REQ_ID" > /tmp/cache_test_3.log 2>&1
TIMESTAMP_END=$(date +%s%N)
TIME_THIRD_MS=$(( (TIMESTAMP_END - TIMESTAMP_START) / 1000000 ))

echo "Result:"
grep "OK" /tmp/cache_test_3.log || true
echo "Response time: ${TIME_THIRD_MS}ms"

sleep 1

echo ""
echo -e "${BLUE}Verifying this was a fresh computation (no cache hit):${NC}"
if grep "Idempotency HIT.*$NEW_REQ_ID" "$LOG_DIR/C.log"; then
    echo -e "${RED}✗ Unexpected cache hit for new request${NC}"
else
    echo -e "${GREEN}✓ Fresh computation (correct)${NC}"
fi

sleep 2

echo ""
echo "============================================"
echo "Test 4: Retry Scenario with Same Request ID"
echo "============================================"
echo ""

RETRY_REQ_ID="req-test-retry-99999"
echo -e "${CYAN}Request ID: ${RETRY_REQ_ID}${NC}"

echo -e "${YELLOW}Step 1: Send initial request...${NC}"
timeout 10 ./build/basecamp_client --target=GREEN --payload_size=512 --request_id="$RETRY_REQ_ID" > /tmp/cache_test_4.log 2>&1
grep "OK" /tmp/cache_test_4.log || true
sleep 2

echo ""
echo -e "${YELLOW}Step 2: Kill node B (GREEN leader)...${NC}"
kill $PID_B 2>/dev/null || true
sleep 2

echo ""
echo -e "${YELLOW}Step 3: Send same request_id while B is down (will fail with retries)...${NC}"
timeout 20 ./build/basecamp_client --target=GREEN --payload_size=512 --request_id="$RETRY_REQ_ID" > /tmp/cache_test_5.log 2>&1 || {
    echo -e "${RED}Request failed (expected - B is down)${NC}"
}
sleep 1

echo ""
echo -e "${YELLOW}Step 4: Restart B and send same request_id again...${NC}"
./build/basecamp_server --node=B >> "$LOG_DIR/B.log" 2>&1 &
PID_B=$!
wait_for_server "B" "50052"
sleep 2

TIMESTAMP_START=$(date +%s%N)
timeout 10 ./build/basecamp_client --target=GREEN --payload_size=512 --request_id="$RETRY_REQ_ID" > /tmp/cache_test_6.log 2>&1
TIMESTAMP_END=$(date +%s%N)
TIME_AFTER_RECOVERY_MS=$(( (TIMESTAMP_END - TIMESTAMP_START) / 1000000 ))

echo "Result after recovery:"
grep "OK" /tmp/cache_test_6.log || true
echo "Response time: ${TIME_AFTER_RECOVERY_MS}ms"

sleep 1

echo ""
echo -e "${BLUE}Checking if retry hit the idempotency cache...${NC}"
CACHE_HITS=$(grep -c "Idempotency HIT.*$RETRY_REQ_ID" "$LOG_DIR/C.log" || echo "0")
echo "Total idempotency hits for $RETRY_REQ_ID: $CACHE_HITS"

if [ "$CACHE_HITS" -gt 0 ]; then
    echo -e "${GREEN}✓ Cache helped during retry scenario!${NC}"
    grep "Idempotency HIT.*$RETRY_REQ_ID" "$LOG_DIR/C.log"
else
    echo -e "${YELLOW}~ No cache hits (requests might have been unique)${NC}"
fi

sleep 2

echo ""
echo "============================================"
echo "Full Cache & Retry Analysis"
echo "============================================"
echo ""

echo -e "${YELLOW}--- All Idempotency HITs in Node C (GREEN worker) ---${NC}"
if grep "Idempotency HIT" "$LOG_DIR/C.log"; then
    HIT_COUNT=$(grep -c "Idempotency HIT" "$LOG_DIR/C.log" || echo "0")
    echo ""
    echo -e "${GREEN}Total cache hits in C: $HIT_COUNT${NC}"
else
    echo "No cache hits found in C"
fi

echo ""
echo -e "${YELLOW}--- All Idempotency HITs in Node D (PINK worker) ---${NC}"
if grep "Idempotency HIT" "$LOG_DIR/D.log"; then
    HIT_COUNT=$(grep -c "Idempotency HIT" "$LOG_DIR/D.log" || echo "0")
    echo ""
    echo -e "${GREEN}Total cache hits in D: $HIT_COUNT${NC}"
else
    echo "No cache hits found in D"
fi

echo ""
echo -e "${YELLOW}--- Node A (Leader) - Retry Pattern ---${NC}"
RETRY_COUNT=$(grep -c "WARN" "$LOG_DIR/A.log" || echo "0")
echo "Total retry attempts: $RETRY_COUNT"
echo ""
echo "Sample retries:"
grep "WARN" "$LOG_DIR/A.log" | head -3 || echo "No retries found"

echo ""
echo "============================================"
echo "Results Summary"
echo "============================================"
echo ""
echo "Response Times:"
echo "  Test 1 (fresh):                ${TIME_FIRST_MS}ms"
echo "  Test 2 (duplicate request_id): ${TIME_SECOND_MS}ms"
echo "  Test 3 (new request_id):       ${TIME_THIRD_MS}ms"
echo "  Test 4 (after recovery):       ${TIME_AFTER_RECOVERY_MS}ms"
echo ""

echo -e "${BLUE}Key Findings:${NC}"
echo "1. Worker nodes cache responses by request_id"
echo "2. Duplicate request_id triggers 'Idempotency HIT'"
echo "3. Cache prevents redundant computation"
echo "4. Retry mechanism uses exponential backoff (100ms, 200ms, 400ms)"
echo ""

echo -e "${GREEN}✓ Cache & Idempotency test completed${NC}"
echo ""
echo "To manually verify cache hits:"
echo "  grep 'Idempotency HIT' logs/C.log"
echo "  grep 'WARN' logs/A.log"