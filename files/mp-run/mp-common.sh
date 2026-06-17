#!/usr/bin/env bash
# Shared config + helpers for the Morrowind MP launch scripts.
# Override any of the paths below by exporting them before running.
set -uo pipefail

OPENMW_ROOT="${OPENMW_ROOT:-$HOME/ai-morrowind/openmw}"
RUN_DIR="${RUN_DIR:-$HOME/ai-morrowind/openmw-run}"
MW_DATA="${MW_DATA:-/home/aaron/.steam/steam/steamapps/common/Morrowind/Data Files}"

OPENMW_BIN="$OPENMW_ROOT/build/openmw"
RESOURCES="$OPENMW_ROOT/build/resources"
VFS_MW="$RESOURCES/vfs-mw"
DATA_DIR="$RUN_DIR/data"
export LD_LIBRARY_PATH="$OPENMW_ROOT/native-lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Set KEEP_SAVES=1 to skip wiping a role's save dir on launch (default: wipe, so the
# char-gen bypass boots a fresh in-world session).
KEEP_SAVES="${KEEP_SAVES:-0}"

die() { echo "[mp] error: $*" >&2; exit 1; }

preflight() {
    [ -x "$OPENMW_BIN" ]   || die "openmw binary not found/executable at $OPENMW_BIN (build it first)"
    [ -d "$MW_DATA" ]      || die "Morrowind Data Files not found at: $MW_DATA (set MW_DATA=...)"
    [ -d "$VFS_MW" ]       || die "vfs-mw overlay not found at $VFS_MW (required, or control scripts crash)"
    [ -d "$DATA_DIR" ]     || die "addon data dir not found at $DATA_DIR"
}

# common_args <config-subdir> <userdata-subdir>  -> echoes the shared openmw args
_common_args=()
_build_common_args() {
    local cfg="$1" ud="$2"
    [ -d "$RUN_DIR/$cfg" ] || die "config dir not found: $RUN_DIR/$cfg"
    [ "$KEEP_SAVES" = "1" ] || rm -rf "$RUN_DIR/$ud/saves" 2>/dev/null
    _common_args=(
        --config   "$RUN_DIR/$cfg"
        --user-data "$RUN_DIR/$ud"
        --resources "$RESOURCES"
        --replace=data --data "$VFS_MW" --data "$MW_DATA" --data "$DATA_DIR"
        --replace=content --content Morrowind.esm
    )
}

# run_server  -> dedicated authority, headless + GPU/display-less (offscreen EGL)
run_server() {
    _build_common_args config-server userdata-server
    echo "[mp] server: headless, offscreen, content=mp-server.omwscripts"
    env -u DISPLAY -u WAYLAND_DISPLAY SDL_VIDEODRIVER=offscreen \
        "$OPENMW_BIN" "${_common_args[@]}" \
        --content mp-server.omwscripts \
        --headless --skip-menu --no-sound --no-grab "$@"
}

# run_client <1|2> [extra args]  -> visible playable client (client 1 or 2)
run_client() {
    local slot="${1:-1}"; shift || true
    local cfg ud
    case "$slot" in
        1) cfg=config-client;  ud=userdata-client  ;;
        2) cfg=config-client2; ud=userdata-client2 ;;
        *) die "client slot must be 1 or 2 (got: $slot)" ;;
    esac
    _build_common_args "$cfg" "$ud"
    echo "[mp] client $slot: visible, content=godmode + mp-client.omwscripts"
    # HEADLESS=1 runs a client without a window (for automated testing)
    if [ "${HEADLESS:-0}" = "1" ]; then
        env -u DISPLAY -u WAYLAND_DISPLAY SDL_VIDEODRIVER=offscreen \
            "$OPENMW_BIN" "${_common_args[@]}" \
            --content godmode.omwscripts --content mp-client.omwscripts \
            --headless --skip-menu --no-sound --no-grab "$@"
    else
        "$OPENMW_BIN" "${_common_args[@]}" \
            --content godmode.omwscripts --content mp-client.omwscripts \
            --skip-menu "$@"
    fi
}
