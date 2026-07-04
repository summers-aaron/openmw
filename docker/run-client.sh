#!/usr/bin/env bash
# Boot a rendering OpenMW client (a real window) and connect it to a multiplayer server by IP.
#
# Usage:
#   docker/run-client.sh <SERVER_IP[:PORT]>
#
# Examples:
#   docker/run-client.sh 192.168.1.50        # connect on the default port (25565)
#   docker/run-client.sh 192.168.1.50:26000  # explicit port
#   docker/run-client.sh 127.0.0.1           # a server on this same machine
#
# Everything else happens in-game: on connect the server offers its stored characters — pick one to
# resume it, or choose "New character" to create one through the normal character-creation intro.
# Run the script twice to test two players; each client gets its own container name and coexists.
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
#   NAME             container name (default openmw-client, auto-numbered) — set for a fixed name
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=docker/_mp-common.sh
source "$REPO/docker/_mp-common.sh"

DEFAULT_PORT="${PORT:-25565}"

[ $# -eq 1 ] || { echo "usage: $(basename "$0") <SERVER_IP[:PORT]>" >&2; exit 1; }
SERVER="$1"
case "$SERVER" in *:*) ;; *) SERVER="$SERVER:$DEFAULT_PORT" ;; esac

mp_common_preflight
mp_common_pick_name openmw-client   # -> NAME (auto-numbered so two clients coexist)
mp_common_config_copy "$NAME"   # -> MP_CFG
mp_common_userdata "$NAME"      # -> MP_USERDATA
mp_common_gpu_render            # -> MP_RENDER, MP_GPU

echo "Connecting client '$NAME' to $SERVER (gpu=$MP_GPU) — a window will open, ~20-40s to load..."
exec "$MP_RUNTIME" run --rm --name "$NAME" --network host --security-opt label=disable \
    -e OPENMW_DEBUG_LEVEL="${OPENMW_DEBUG_LEVEL:-INFO}" \
    "${MP_RENDER[@]}" \
    -v "$REPO:/openmw:Z" "${MP_DATA_MOUNTS[@]}" -v "$MP_CFG:/root/.config/openmw" -v "$MP_USERDATA:/userdata" \
    -w /openmw/build "$IMAGE" \
    bash -lc "./openmw --resources resources --skip-menu --no-grab --user-data /userdata \
        --connect $SERVER"
