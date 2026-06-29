#!/usr/bin/env bash
# Boot a rendering OpenMW client (a real window) and connect it to a multiplayer server by IP.
#
# Usage:
#   docker/run-client.sh <SERVER_IP[:PORT]> [extra openmw args...]
#
# Examples:
#   docker/run-client.sh 192.168.1.50                  # connect on the default port (25565)
#   docker/run-client.sh 192.168.1.50:26000            # explicit port
#   docker/run-client.sh 127.0.0.1                      # a server on this same machine
#   OPENMW_USERDATA=~/saves docker/run-client.sh 10.0.0.2 --load-savegame /userdata/saves/x.omwsave
#
# GPU is auto-detected (NVIDIA if /dev/nvidia0 exists, else Mesa via /dev/dri); force with GPU=mesa|nvidia.
# With no game/save arg it starts a new game. Run two clients (different NAME) to test two players.
#
# Env overrides:
#   OPENMW_DATA      Morrowind "Data Files" path (must match your openmw.cfg data= entry)
#   OPENMW_CONFIG    openmw config dir (default ~/.config/openmw)
#   OPENMW_USERDATA  a dir to mount at /userdata (for loading saves); default: throwaway temp dir
#   IMAGE            build image (default openmw.server:latest)
#   CONTAINER_RUNTIME  podman|docker (auto-detected)
#   PORT             default server port when none is given in the address (default 25565)
#   GPU              auto|mesa|nvidia (default auto)
#   NAME             container name (default openmw-client) — use distinct names for multiple clients
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=docker/_mp-common.sh
source "$REPO/docker/_mp-common.sh"

NAME="${NAME:-openmw-client}"
DEFAULT_PORT="${PORT:-25565}"

[ $# -ge 1 ] || { echo "usage: $(basename "$0") <SERVER_IP[:PORT]> [extra openmw args...]" >&2; exit 1; }
SERVER="$1"; shift
case "$SERVER" in *:*) ;; *) SERVER="$SERVER:$DEFAULT_PORT" ;; esac

mp_common_preflight
mp_common_config_copy "$NAME"   # -> MP_CFG
mp_common_userdata "$NAME"      # -> MP_USERDATA
mp_common_gpu_render            # -> MP_RENDER, MP_GPU

# Default to a new game when no game/save selector was passed through.
game=(--new-game)
for a in "$@"; do
    case "$a" in --new-game|--load-savegame|--skip-menu) game=() ;; esac
done

"$MP_RUNTIME" rm -f "$NAME" >/dev/null 2>&1 || true
echo "Connecting client '$NAME' to $SERVER (gpu=$MP_GPU) — a window will open, ~20-40s to load..."
exec "$MP_RUNTIME" run --rm --name "$NAME" --network host --security-opt label=disable \
    "${MP_RENDER[@]}" \
    -v "$REPO:/openmw:Z" -v "$MP_DATA:$MP_DATA:ro" -v "$MP_CFG:/root/.config/openmw" -v "$MP_USERDATA:/userdata" \
    -w /openmw/build "$IMAGE" \
    bash -lc "./openmw --resources resources --no-grab --user-data /userdata \
        ${game[*]:-} --connect $SERVER $*"
