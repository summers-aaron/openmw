# Shared helpers for docker/run-server.sh and docker/run-client.sh. Sourced, not executed.
#
# Defines the common env (with overrides) and a few helper functions so the launchers stay small.
# Host-specific paths default to this machine but are all overridable via env.

IMAGE="${IMAGE:-openmw.server:latest}"
# Morrowind game data. MUST match the data= path in your openmw.cfg, because the container mounts
# it at the same path so the config resolves inside the container too.
MP_DATA="${OPENMW_DATA:-/home/aaron/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/Morrowind/Data Files}"
MP_CONFIG="${OPENMW_CONFIG:-$HOME/.config/openmw}"

# --- container runtime: explicit override, else podman (Fedora), else docker ---
MP_RUNTIME="${CONTAINER_RUNTIME:-}"
if [ -z "$MP_RUNTIME" ]; then
    if command -v podman >/dev/null 2>&1; then MP_RUNTIME=podman
    elif command -v docker >/dev/null 2>&1; then MP_RUNTIME=docker
    else echo "error: no podman or docker found" >&2; exit 1; fi
fi

mp_common_preflight() {
    command -v "$MP_RUNTIME" >/dev/null 2>&1 || { echo "error: '$MP_RUNTIME' not found" >&2; exit 1; }
    if ! "$MP_RUNTIME" image inspect "$IMAGE" >/dev/null 2>&1; then
        echo "error: image '$IMAGE' not found — build it first with: docker/build-host.sh" >&2
        exit 1
    fi
    if [ ! -d "$MP_DATA" ]; then
        echo "error: Morrowind Data Files not found at:" >&2
        echo "  $MP_DATA" >&2
        echo "  Set OPENMW_DATA to the right path (it must match the data= line in your openmw.cfg)." >&2
        exit 1
    fi
    [ -d "$MP_CONFIG" ] || { echo "error: openmw config dir not found at: $MP_CONFIG (set OPENMW_CONFIG)" >&2; exit 1; }
}

# Per-instance writable copy of the config, so each process can write settings/MyGUI.log without
# fighting over the shared one. $1 = instance name -> sets MP_CFG.
mp_common_config_copy() {
    MP_CFG="/tmp/openmw-mp-$1-cfg"
    rm -rf "$MP_CFG"; cp -r "$MP_CONFIG" "$MP_CFG"
}

# A user-data dir to mount at /userdata (holds saves). If OPENMW_USERDATA is set it is COPIED (so the
# original is never mutated); otherwise a fresh empty dir is used. $1 = instance name -> sets MP_USERDATA.
mp_common_userdata() {
    MP_USERDATA="/tmp/openmw-mp-$1-ud"
    rm -rf "$MP_USERDATA"
    if [ -n "${OPENMW_USERDATA:-}" ]; then
        [ -d "$OPENMW_USERDATA" ] || { echo "error: OPENMW_USERDATA is not a directory: $OPENMW_USERDATA" >&2; exit 1; }
        cp -r "$OPENMW_USERDATA" "$MP_USERDATA"
    else
        mkdir -p "$MP_USERDATA"
    fi
}

# Build the run flags for a RENDERING client (a real window): GPU + X11 display + audio.
# GPU is auto-detected (nvidia if /dev/nvidia0 exists, else mesa); override with GPU=mesa|nvidia.
# Sets MP_RENDER (array of run flags) and MP_GPU (the chosen mode).
mp_common_gpu_render() {
    local disp="${DISPLAY:-:0}"
    xhost +local: >/dev/null 2>&1 || echo "warning: xhost failed — if no window appears, run 'xhost +local:' yourself" >&2
    MP_RENDER=(-e DISPLAY="$disp" -e SDL_VIDEODRIVER=x11 -v /tmp/.X11-unix:/tmp/.X11-unix)
    [ -e /dev/dri ] && MP_RENDER+=(--device /dev/dri)

    MP_GPU="${GPU:-auto}"
    [ "$MP_GPU" = auto ] && { [ -e /dev/nvidia0 ] && MP_GPU=nvidia || MP_GPU=mesa; }
    if [ "$MP_GPU" = nvidia ]; then
        # The image ships only mesa GL; stage the host's NVIDIA GL/GLX userspace into a dir we
        # bind-mount, and route GLVND to it. Library dir differs by distro (Fedora vs Debian/Ubuntu).
        local libdir=/usr/lib64
        [ -e "$libdir/libGLX_nvidia.so.0" ] || libdir=/usr/lib/x86_64-linux-gnu
        local stage=/tmp/openmw-mp-nvidia/lib
        rm -rf /tmp/openmw-mp-nvidia; mkdir -p "$stage"
        cp -a "$libdir"/libnvidia-*.so*   "$stage/" 2>/dev/null || true
        cp -a "$libdir"/libEGL_nvidia.so* "$stage/" 2>/dev/null || true
        cp -a "$libdir"/libGLX_nvidia.so* "$stage/" 2>/dev/null || true
        local d
        for d in /dev/nvidia0 /dev/nvidiactl /dev/nvidia-modeset /dev/nvidia-uvm /dev/nvidia-uvm-tools; do
            [ -e "$d" ] && MP_RENDER+=(--device "$d")
        done
        MP_RENDER+=(-e LD_LIBRARY_PATH=/nvidia/lib -e __GLX_VENDOR_LIBRARY_NAME=nvidia -v "$stage:/nvidia/lib:ro")
    fi

    local rt="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
    if [ -S "$rt/pulse/native" ]; then
        MP_RENDER+=(-v "$rt/pulse/native:$rt/pulse/native" -e PULSE_SERVER="unix:$rt/pulse/native")
    else
        echo "warning: no PulseAudio socket at $rt/pulse/native — client will be silent" >&2
    fi
}
