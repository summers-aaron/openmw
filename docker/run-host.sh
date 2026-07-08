#!/usr/bin/env bash
# Boot a LISTEN HOST: a rendering OpenMW client (a real window, playable host player) that also
# hosts the multiplayer session. The host player IS the authority — handy for debugging, since you
# can stand in the shared world and inspect server-side state directly (e.g. F10 navmesh view).
#
# Run docker/run-client.sh on the same or another machine to join it.
#
# Note: a listen host is close to, but not identical to, the dedicated server (it runs with a real
# primary player instead of a headless placeholder). For faithful dedicated-server repros use
# docker/run-server.sh; use this to play along or eyeball authority-side state.
#
# Usage:
#   docker/run-host.sh                       # new game, default port 25565
#   docker/run-host.sh --listen 26000        # custom port
#   docker/run-host.sh --save ~/openmw-mp-server-data/saves/test/MP.omwsave   # host a save
#   docker/run-host.sh --start "Seyda Neen"  # skip chargen: instant default debug player there
#
# GPU is auto-detected (NVIDIA if /dev/nvidia0 exists, else Mesa via /dev/dri); force with GPU=mesa|nvidia.
# There is no terminal console (that is dedicated-only) — use the in-game console instead.
#
# Env overrides:
#   OPENMW_DATA      Morrowind "Data Files" path (must match your openmw.cfg data= entry)
#   OPENMW_CONFIG    openmw config dir (default ~/.config/openmw)
#   OPENMW_USERDATA  PERSISTENT dir mounted read-write at /userdata; shared with run-server.sh by
#                    default (~/openmw-mp-server-data) so saves carry between host and server runs
#   OPENMW_SAVE      a host .omwsave to host (same as --save)
#   IMAGE            build image (default openmw.server:latest)
#   CONTAINER_RUNTIME  podman|docker (auto-detected)
#   PORT             listen port when --listen isn't passed (default 25565)
#   GPU              auto|mesa|nvidia (default auto)
#   NAME             container name (default openmw-host, auto-numbered) — set for a fixed name
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
mp_common_pick_name openmw-host   # -> NAME (auto-numbered so multiple instances coexist)
mp_common_config_copy "$NAME"   # -> $MP_CFG
mp_common_gpu_render            # -> MP_RENDER, MP_GPU

# The host is the authority, so its user-data is PERSISTENT like the dedicated server's (and shares
# its default dir, so a save made under run-server.sh can be hosted here and vice versa).
MP_USERDATA="${OPENMW_USERDATA:-$HOME/openmw-mp-server-data}"
mkdir -p "$MP_USERDATA"
echo "Host user-data (saves persist here): $MP_USERDATA"

mp_common_save "$SAVE_HOST"     # -> MP_SAVE_MOUNT, MP_SAVE_LOAD (a host .omwsave to host)

# Choose the game: an explicit --save loads that save; otherwise default to a new game unless the
# pass-through already carries its own game/save selector. --start counts as one: combined with the
# hardcoded --skip-menu (and no --new-game) it bypasses chargen and drops a default debug player
# straight into that cell, e.g.:  docker/run-host.sh --start "Seyda Neen"
if [ ${#MP_SAVE_LOAD[@]} -gt 0 ]; then
    game=("${MP_SAVE_LOAD[@]}")
else
    game=(--new-game)
    for a in ${passthrough[@]+"${passthrough[@]}"}; do
        case "$a" in --new-game|--load-savegame|--skip-menu|--start) game=() ;; esac
    done
fi

# Pre-quote the pass-through: it is spliced into a bash -lc string, so args with spaces
# (--start "Seyda Neen") would otherwise be word-split inside the container.
pt=""
[ ${#passthrough[@]} -gt 0 ] && pt="$(printf '%q ' "${passthrough[@]}")"

echo "Starting listen host '$NAME' on port $LISTEN (gpu=$MP_GPU) — a window will open, ~20-40s to load..."
exec "$MP_RUNTIME" run --rm --name "$NAME" --network host --security-opt label=disable \
    -e OPENMW_DEBUG_LEVEL="${OPENMW_DEBUG_LEVEL:-INFO}" \
    "${MP_RENDER[@]}" \
    -v "$REPO:/openmw:Z" "${MP_DATA_MOUNTS[@]}" -v "$MP_CFG:/root/.config/openmw" -v "$MP_USERDATA:/userdata" \
    ${MP_SAVE_MOUNT[@]+"${MP_SAVE_MOUNT[@]}"} \
    -w /openmw/build "$IMAGE" \
    bash -lc "./openmw --resources resources --skip-menu --no-grab --listen $LISTEN \
        --user-data /userdata ${game[*]:-} $pt"
