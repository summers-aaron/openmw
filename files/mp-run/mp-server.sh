#!/usr/bin/env bash
# Start the Morrowind MP dedicated server (headless, authoritative, GPU/display-less).
#   ./mp-server.sh                # run in this terminal
#   KEEP_SAVES=1 ./mp-server.sh   # don't wipe the server save on launch
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/mp-common.sh"
preflight
run_server "$@"
