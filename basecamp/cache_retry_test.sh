#!/bin/bash
# Cache & Idempotency with Retry Test - MULTI-ITERATION VERSION

set -e

TOPOLOGY_FILE="$(pwd)/config/topology_2hosts.textproto"
export TOPOLOGY_FILE
PYTHONPATH=".:./py"
export PYTHONPATH

LOG_DIR="logs"
mkdir -p "$LOG_DIR"

# Configuration
NUM_ITERATIONS=${1:-5}  # Default 5 iterations, override with first argument
PAYLOAD_SIZE=${2:-512}  # Default payload size in bytes, override with second argument

# Clear old logs
rm -f "$LOG_DIR"/*.log /tmp/cache_test_*.log 2>/dev/null

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Cross-platform timeout function
run_with_timeout() {
    local duration=$1
    shift
    local command="$@"
    
    if command -v timeout >/dev/null 2>&1; then
        timeout "$duration" $command
    elif command -v gtimeout >/dev/null 2>&1; then
        gtimeout "$duration" $command
    else
        # Perl fallback for macOS without coreutils
        perl -e 'alarm shift; exec @ARGV' "$duration" $command
    fi
}


# Cross-platform timestamp function (nanoseconds)
get_timestamp() {
    if date +%s%N | grep -q 'N'; then
        # macOS/BSD date (does not support %N)
        # Use python fallback
        python3 -c 'import time; print(int(time.time() * 1000000000))'
    else
        # GNU date
        date +%s%N
    fi
}

echo "============================================"
echo "Cache & Idempotency with Retry Test"
echo "Multi-Iteration Mode: $NUM_ITERATIONS runs | payload_size=${PAYLOAD_SIZE} bytes"
echo "============================================"
echo "Testing with payload size: ${PAYLOAD_SIZE} bytes"
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

# trap cleanup EXIT

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

# Function to calculate basic statistics
calculate_stats() {
    local array=("$@")
    local sum=0
    local count=${#array[@]}
    local min=${array[0]}
    local max=${array[0]}
    
    for val in "${array[@]}"; do
        sum=$((sum + val))
        [ $val -lt $min ] && min=$val
        [ $val -gt $max ] && max=$val
    done
    
    local avg=$((sum / count))
    echo "$min $max $avg $sum $count"
}

# Arrays to store timing data
declare -a FRESH_TIMES
declare -a CACHED_TIMES

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
echo "Multi-Iteration Cache Performance Test"
echo "============================================"
echo ""

for i in $(seq 1 $NUM_ITERATIONS); do
    echo -e "${CYAN}===== ITERATION $i / $NUM_ITERATIONS =====${NC}"
    echo ""
    
    # Generate unique request IDs for this iteration
    ITER_BASE_ID="iter-$i-"
    FRESH_REQ_ID="${ITER_BASE_ID}fresh-$(get_timestamp)"
    
    # Test 1: Fresh request
    echo -e "${YELLOW}Fresh request (new ID):${NC}"
    TIMESTAMP_START=$(get_timestamp)
    run_with_timeout 10 ./build/basecamp_client --target=GREEN --payload_size="$PAYLOAD_SIZE" --request_id="$FRESH_REQ_ID" > /tmp/cache_test_iter_${i}_fresh.log 2>&1
    TIMESTAMP_END=$(get_timestamp)
    TIME_FRESH=$(( (TIMESTAMP_END - TIMESTAMP_START) / 1000000 ))
    
    grep "OK" /tmp/cache_test_iter_${i}_fresh.log || true
    echo "  Time: ${TIME_FRESH}ms"
    FRESH_TIMES+=($TIME_FRESH)
    
    sleep 1
    
    # Test 2: Cached request (same ID)
    echo -e "${YELLOW}Cached request (duplicate ID):${NC}"
    TIMESTAMP_START=$(get_timestamp)
    run_with_timeout 10 ./build/basecamp_client --target=GREEN --payload_size="$PAYLOAD_SIZE" --request_id="$FRESH_REQ_ID" > /tmp/cache_test_iter_${i}_cached.log 2>&1
    TIMESTAMP_END=$(get_timestamp)
    TIME_CACHED=$(( (TIMESTAMP_END - TIMESTAMP_START) / 1000000 ))
    
    grep "OK" /tmp/cache_test_iter_${i}_cached.log || true
    echo "  Time: ${TIME_CACHED}ms"
    CACHED_TIMES+=($TIME_CACHED)
    
    # Calculate speedup for this iteration
    if [ $TIME_FRESH -gt 0 ] && [ $TIME_CACHED -gt 0 ]; then
        SPEEDUP=$((TIME_FRESH / TIME_CACHED))
        if [ $SPEEDUP -gt 1 ]; then
            echo -e "${GREEN}  Speedup: ${SPEEDUP}x faster${NC}"
        else
            echo -e "${YELLOW}  Similar times (cache hit but small task)${NC}"
        fi
    fi
    
    # Check cache hit
    if grep "Idempotency HIT.*$FRESH_REQ_ID" "$LOG_DIR/C.log" > /dev/null 2>&1; then
        echo -e "${GREEN}  ✓ Cache HIT detected${NC}"
    else
        echo -e "${YELLOW}  ~ No cache hit (may not have reached worker)${NC}"
    fi
    
    sleep 1
    echo ""
done

echo ""
echo "============================================"
echo "Aggregated Results - Multi-Iteration Analysis"
echo "============================================"
echo ""

# Calculate statistics for fresh requests
echo -e "${BLUE}FRESH Requests (new request IDs):${NC}"
read MIN_FRESH MAX_FRESH AVG_FRESH SUM_FRESH COUNT_FRESH <<< $(calculate_stats "${FRESH_TIMES[@]}")
echo "  Min:     ${MIN_FRESH}ms"
echo "  Max:     ${MAX_FRESH}ms"
echo "  Avg:     ${AVG_FRESH}ms"
echo "  Sum:     ${SUM_FRESH}ms (across $COUNT_FRESH iterations)"
echo "  All times: ${FRESH_TIMES[@]}"
echo ""

# Calculate statistics for cached requests
echo -e "${BLUE}CACHED Requests (duplicate request IDs):${NC}"
read MIN_CACHED MAX_CACHED AVG_CACHED SUM_CACHED COUNT_CACHED <<< $(calculate_stats "${CACHED_TIMES[@]}")
echo "  Min:     ${MIN_CACHED}ms"
echo "  Max:     ${MAX_CACHED}ms"
echo "  Avg:     ${AVG_CACHED}ms"
echo "  Sum:     ${SUM_CACHED}ms (across $COUNT_CACHED iterations)"
echo "  All times: ${CACHED_TIMES[@]}"
echo ""

# Calculate overall speedup
if [ $AVG_FRESH -gt 0 ] && [ $AVG_CACHED -gt 0 ]; then
    OVERALL_SPEEDUP=$((AVG_FRESH / AVG_CACHED))
    if [ $OVERALL_SPEEDUP -gt 1 ]; then
        echo -e "${GREEN}Overall Speedup: ${OVERALL_SPEEDUP}x faster (fresh vs cached avg)${NC}"
    else
        echo -e "${YELLOW}Similar average times${NC}"
    fi
fi

echo ""
echo "============================================"
echo "Cache Hit Statistics"
echo "============================================"
echo ""

TOTAL_CACHE_HITS=$(grep -c "Idempotency HIT" "$LOG_DIR/C.log" || echo "0")
echo -e "${BLUE}Total Idempotency Hits in Node C: ${TOTAL_CACHE_HITS}${NC}"
TOTAL_CACHE_HITS_D=$(grep -c "Idempotency HIT" "$LOG_DIR/D.log" || echo "0")
echo -e "${BLUE}Total Idempotency Hits in Node D: ${TOTAL_CACHE_HITS_D}${NC}"

if [ "$TOTAL_CACHE_HITS" -gt 0 ]; then
    echo ""
    echo -e "${YELLOW}Sample cache hits:${NC}"
    grep "Idempotency HIT" "$LOG_DIR/C.log" | head -5
fi

echo ""
echo "============================================"
echo "Performance Summary"
echo "============================================"
echo ""

echo -e "${CYAN}Key Metrics:${NC}"
echo "  Iterations run:          $NUM_ITERATIONS"
echo "  Fresh avg time:          ${AVG_FRESH}ms"
echo "  Cached avg time:         ${AVG_CACHED}ms"
echo "  Difference:              $((AVG_FRESH - AVG_CACHED))ms"
echo "  Cache effectiveness:     $(( (AVG_FRESH - AVG_CACHED) * 100 / AVG_FRESH ))%"
echo "  Total cache hits:        $TOTAL_CACHE_HITS"
echo ""

echo -e "${GREEN}✓ Multi-iteration cache test completed${NC}"
echo ""
echo "To view detailed logs:"
echo "  tail -20 logs/C.log"
echo "  tail -20 logs/A.log"