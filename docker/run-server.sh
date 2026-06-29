#!/usr/bin/env bash
# Boot a headless, dedicated OpenMW multiplayer server that clients connect to over TCP.
#
# The server renders nothing (software GL, offscreen) — it just arbitrates the shared world.
# Run docker/run-client.sh on the same or another machine to join it.
#
# Usage:
#   docker/run-server.sh                       # new game, default port 25565
#   docker/run-server.sh --listen 26000        # custom port
#   docker/run-server.sh --load-savegame /userdata/saves/test/MP.omwsave   # host a save
#                                              # (set OPENMW_USERDATA so /userdata is mounted)
#
# Env overrides:
#   OPENMW_DATA      Morrowind "Data Files" path (must match your openmw.cfg data= entry)
#   OPENMW_CONFIG    openmw config dir (default ~/.config/openmw)
#   OPENMW_USERDATA  a dir to mount at /userdata (for saves); default: a throwaway temp dir
#   IMAGE            build image (default openmw.server:latest)
#   CONTAINER_RUNTIME  podman|docker (auto-detected)
#   PORT             listen port when --listen isn't passed (default 25565)
#   NAME             container name (default openmw-server)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=docker/_mp-common.sh
source "$REPO/docker/_mp-common.sh"

LISTEN="${PORT:-25565}"

# Pull --listen / --save out of the args (so we can act on them); leave everything else as pass-through.
SAVE_HOST="${OPENMW_SAVE:-}"
passthrough=()
while [ $# -gt 0 ]; do
    case "$1" in
        --listen) LISTEN="$2"; shift 2 ;;
        --save)   SAVE_HOST="$2"; shift 2 ;;
        *) passthrough+=("$1"); shift ;;
    esac
done

mp_common_preflight
mp_common_pick_name openmw-server   # -> NAME (auto-numbered so multiple servers coexist)
mp_common_config_copy "$NAME"   # -> $MP_CFG
mp_common_userdata "$NAME"      # -> $MP_USERDATA
mp_common_save "$SAVE_HOST"     # -> MP_SAVE_MOUNT, MP_SAVE_LOAD (a host .omwsave to host)

# Choose the game: an explicit --save loads that save; otherwise default to a new game unless the
# pass-through already carries its own game/save selector.
if [ ${#MP_SAVE_LOAD[@]} -gt 0 ]; then
    game=("${MP_SAVE_LOAD[@]}")
else
    game=(--new-game)
    for a in ${passthrough[@]+"${passthrough[@]}"}; do
        case "$a" in --new-game|--load-savegame|--skip-menu) game=() ;; esac
    done
fi

echo "Starting dedicated server '$NAME' on port $LISTEN (Ctrl+C to stop)..."
exec "$MP_RUNTIME" run --rm --name "$NAME" --network host --security-opt label=disable \
    -e LIBGL_ALWAYS_SOFTWARE=1 -e GALLIUM_DRIVER=llvmpipe -e SDL_VIDEODRIVER=offscreen -e EGL_PLATFORM=surfaceless \
    -v "$REPO:/openmw:Z" -v "$MP_DATA:$MP_DATA:ro" -v "$MP_CFG:/root/.config/openmw" -v "$MP_USERDATA:/userdata" \
    ${MP_SAVE_MOUNT[@]+"${MP_SAVE_MOUNT[@]}"} \
    -w /openmw/build "$IMAGE" \
    bash -lc "./openmw --resources resources --skip-menu --no-grab --dedicated --listen $LISTEN \
        --user-data /userdata ${game[*]:-} ${passthrough[*]:-}"
