#!/usr/bin/env bash
# Start a Morrowind MP client (visible, playable). Connects to 127.0.0.1:7000 by default
# (edit host/port in scripts/mp/config.lua).
#   ./mp-client.sh        # client slot 1
#   ./mp-client.sh 2      # client slot 2 (second player on this machine)
#   HEADLESS=1 ./mp-client.sh 2   # windowless client (for testing)
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/mp-common.sh"
preflight
run_client "${1:-1}" "${@:2}"
