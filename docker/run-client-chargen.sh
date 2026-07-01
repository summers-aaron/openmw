#!/usr/bin/env bash
# Boot a rendering OpenMW client (a real window) and connect it to a multiplayer server, forcing the
# character-creation start path: it joins with NO save and NO onboard script, so the engine runs the
# normal new-game intro (prison ship, census office, character generation) on connect — each client
# creates its own character the vanilla way. This is the chargen counterpart of run-client.sh.
#
# Usage:
#   docker/run-client-chargen.sh <SERVER_IP[:PORT]> [extra openmw args...]
#
# Examples:
#   docker/run-client-chargen.sh 127.0.0.1            # a server on this same machine
#   docker/run-client-chargen.sh 192.168.1.50:26000   # explicit port
#   NAME=c2 docker/run-client-chargen.sh 10.0.0.2     # a second coexisting client
#
# GPU is auto-detected (NVIDIA if /dev/nvidia0 exists, else Mesa via /dev/dri); force with GPU=mesa|nvidia.
#
# Env overrides:
#   OPENMW_DATA      Morrowind "Data Files" path (must match your openmw.cfg data= entry)
#   OPENMW_CONFIG    openmw config dir (default ~/.config/openmw)
#   IMAGE            build image (default openmw.server:latest)
#   CONTAINER_RUNTIME  podman|docker (auto-detected)
#   PORT             default server port when none is given in the address (default 25565)
#   GPU              auto|mesa|nvidia (default auto)
#   NAME             container name (default openmw-client) — use distinct names for multiple clients
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=docker/_mp-common.sh
source "$REPO/docker/_mp-common.sh"

DEFAULT_PORT="${PORT:-25565}"

[ $# -ge 1 ] || { echo "usage: $(basename "$0") <SERVER_IP[:PORT]> [extra openmw args...]" >&2; exit 1; }
SERVER="$1"; shift
case "$SERVER" in *:*) ;; *) SERVER="$SERVER:$DEFAULT_PORT" ;; esac

# Anything left over is passed straight through to openmw.
passthrough=("$@")

mp_common_preflight
mp_common_pick_name openmw-client   # -> NAME (auto-numbered so two clients coexist)
mp_common_config_copy "$NAME"   # -> MP_CFG
mp_common_userdata "$NAME"      # -> MP_USERDATA
mp_common_gpu_render            # -> MP_RENDER, MP_GPU

# No save and no onboard/startup script: with no --load-savegame the engine takes the multiplayer
# chargen path on connect (a --script-run onboard would set ChargenState and teleport, short-circuiting
# chargen — so it is deliberately omitted here).
echo "Connecting client '$NAME' to $SERVER (gpu=$MP_GPU, chargen) — a window will open, ~20-40s to load..."
exec "$MP_RUNTIME" run --rm --name "$NAME" --network host --security-opt label=disable \
    "${MP_RENDER[@]}" \
    -v "$REPO:/openmw:Z" "${MP_DATA_MOUNTS[@]}" -v "$MP_CFG:/root/.config/openmw" -v "$MP_USERDATA:/userdata" \
    -w /openmw/build "$IMAGE" \
    bash -lc "./openmw --resources resources --skip-menu --no-grab --user-data /userdata \
        --connect $SERVER ${passthrough[*]:-}"
