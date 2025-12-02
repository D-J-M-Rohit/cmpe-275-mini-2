#!/bin/bash
set -e
set -x

# Kill any existing processes
pkill -f basecamp_server || true
pkill -f server_e.py || true
pkill -f server_f.py || true

# Export topology
export TOPOLOGY_FILE="$(pwd)/config/topology_2hosts.textproto"

# Activate Python venv
if [ -d ".venv" ]; then
    source .venv/bin/activate
else
    echo "Warning: .venv not found"
fi

echo "Starting servers..."

# Start C++ servers
./build/basecamp_server --node=A > logs/A.log 2>&1 &
PID_A=$!
./build/basecamp_server --node=B > logs/B.log 2>&1 &
PID_B=$!
./build/basecamp_server --node=C > logs/C.log 2>&1 &
PID_C=$!
./build/basecamp_server --node=D > logs/D.log 2>&1 &
PID_D=$!

# Start Python servers
export PYTHONPATH=".:./py"
python3 -m py.server_e > logs/E.log 2>&1 &
PID_E=$!
python3 -m py.server_f > logs/F.log 2>&1 &
PID_F=$!

echo "Servers started. PIDs: A=$PID_A, B=$PID_B, C=$PID_C, D=$PID_D, E=$PID_E, F=$PID_F"
echo "Waiting 5s for startup..."
sleep 5

# Check if processes are still running
if ! ps -p $PID_A > /dev/null; then echo "A died"; cat logs/A.log; exit 1; fi
if ! ps -p $PID_B > /dev/null; then echo "B died"; cat logs/B.log; exit 1; fi
if ! ps -p $PID_E > /dev/null; then echo "E died"; cat logs/E.log; exit 1; fi

# Run Test Client
echo "Running Test Client..."
python3 py/test_chunked_client.py

# Cleanup
echo "Stopping servers..."
kill $PID_A $PID_B $PID_C $PID_D $PID_E $PID_F
wait $PID_A $PID_B $PID_C $PID_D $PID_E $PID_F 2>/dev/null || true
echo "Done."
