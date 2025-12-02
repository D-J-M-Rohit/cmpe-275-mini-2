#!/bin/bash
set -e

# Stress Test: Run multiple concurrent clients to test system under load
# This verifies the system handles concurrent requests correctly

echo "========================================="
echo "Running Stress Test with Concurrent Clients"
echo "========================================="

NUM_CLIENTS=${1:-10}  # Default 10 concurrent clients
echo "Launching $NUM_CLIENTS concurrent test clients..."

# Function to run a single client
run_client() {
    local client_id=$1
    local output_file="logs/stress_client_${client_id}.log"
    python3 py/test_chunked_client.py > "$output_file" 2>&1
    local exit_code=$?
    if [ $exit_code -eq 0 ]; then
        echo "✓ Client $client_id completed successfully"
    else
        echo "✗ Client $client_id failed (exit code: $exit_code)"
    fi
    return $exit_code
}

# Create logs directory
mkdir -p logs

# Launch clients in parallel
pids=()
for i in $(seq 1 $NUM_CLIENTS); do
    run_client $i &
    pids+=($!)
done

# Wait for all clients and collect results
echo "Waiting for all clients to complete..."
failed=0
for pid in "${pids[@]}"; do
    if ! wait "$pid"; then
        failed=$((failed + 1))
    fi
done

echo "========================================="
echo "Stress Test Complete"
echo "Total clients: $NUM_CLIENTS"
echo "Successful: $((NUM_CLIENTS - failed))"
echo "Failed: $failed"
echo "========================================="

if [ $failed -gt 0 ]; then
    echo "⚠️  Some clients failed. Check logs/stress_client_*.log for details"
    exit 1
else
    echo "✅ All clients passed!"
    exit 0
fi
