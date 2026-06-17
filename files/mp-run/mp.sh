#!/usr/bin/env bash
# Start the dedicated server (background) + one client (this terminal) together.
# Server log goes to $RUN_DIR/server.log. Closing the client stops the server.
#   ./mp.sh        # server + client slot 1
#   ./mp.sh 2      # server + client slot 2
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/mp-common.sh"
preflight

SERVER_LOG="$RUN_DIR/server.log"
echo "[mp] starting dedicated server (log: $SERVER_LOG)"
run_server > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!
trap 'echo "[mp] stopping server (pid $SERVER_PID)"; kill "$SERVER_PID" 2>/dev/null' EXIT INT TERM

# wait (up to ~30s) for the server to start listening
for _ in $(seq 1 60); do
    kill -0 "$SERVER_PID" 2>/dev/null || die "server exited early — see $SERVER_LOG"
    if grep -q "server listening" "$SERVER_LOG" 2>/dev/null; then break; fi
    sleep 0.5
done
echo "[mp] server up. launching client ${1:-1}..."
run_client "${1:-1}"
