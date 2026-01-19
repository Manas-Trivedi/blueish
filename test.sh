#!/usr/bin/env bash
set -euo pipefail

SERVER_BIN=${1:-./server}
CLIENT_BIN=${2:-./client}
NUM_CLIENTS=${3:-5}
PORT=${4:-1234}

echo "[*] Starting server..."
$SERVER_BIN &
SERVER_PID=$!

# Always clean up server
cleanup() {
    echo
    echo "[*] Killing server (pid=$SERVER_PID)"
    kill $SERVER_PID 2>/dev/null || true
}
trap cleanup EXIT

# Wait until server is accepting connections
echo "[*] Waiting for server to listen on port $PORT..."
for i in {1..50}; do
    if nc -z 127.0.0.1 $PORT 2>/dev/null; then
        break
    fi
    sleep 0.1
done

if ! nc -z 127.0.0.1 $PORT 2>/dev/null; then
    echo "[!] Server did not start listening"
    exit 1
fi

echo "[*] Server is up"

# Barrier: start all clients at the same time
echo "[*] Spawning $NUM_CLIENTS concurrent clients..."

pids=()
for i in $(seq 1 $NUM_CLIENTS); do
    (
        echo "[client $i] start"
        $CLIENT_BIN
        echo "[client $i] done"
    ) &
    pids+=($!)
done

# Wait for all clients
for pid in "${pids[@]}"; do
    wait $pid
done

echo "[✓] All clients finished successfully"