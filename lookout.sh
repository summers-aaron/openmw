#!/usr/bin/env bash
set -uo pipefail
cd /openmw/build
export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe SDL_VIDEODRIVER=offscreen EGL_PLATFORM=surfaceless OPENMW_DEBUG_LEVEL=INFO
H=/openmw/build/lk-host.log; C=/openmw/build/lk-client.log
F=/tmp/hfifo; mkfifo "$F"; exec 9<>"$F"
./openmw --resources resources --skip-menu --no-grab --dedicated --listen 25565 --user-data /userdata-host --start "Seyda Neen" < "$F" > "$H" 2>&1 &
HOST=$!
for i in $(seq 1 40); do grep -q "Server console ready" "$H" 2>/dev/null && break; sleep 1; done
echo 'set gamehour to 23' >&9; sleep 1
echo 'Journal "MS_Lookout" 20' >&9; sleep 1
echo 'COC "Balmora"' >&9; sleep 3
./openmw --resources resources --skip-menu --no-grab --user-data /userdata-client --connect 127.0.0.1:25565 --character -2 > "$C" 2>&1 &
CLIENT=$!
for i in $(seq 1 90); do grep -q "LOOKOUTTEST" "$C" 2>/dev/null && break; sleep 1; done
for r in $(seq 1 12); do echo 'Fargoth->GetPos X' >&9; echo 'Fargoth->GetPos Y' >&9; echo 'Fargoth->GetForceSneak' >&9; sleep 4; done
kill -9 $HOST $CLIENT 2>/dev/null
echo "HARNESS_COMPLETE" >> "$H"
