#!/usr/bin/env bash
# Host-side wrapper: build OpenMW inside the container, incrementally.
#
# The build/ cache is configured for the in-container source path (/openmw), so a direct
# host `make` fails ("source directory /openmw does not exist"). This drives the container
# instead. Works with podman (Fedora default, rootless + SELinux) or docker (WSL/Ubuntu).
#
# Usage:
#   docker/build-host.sh                 # incremental build of the openmw binary (default)
#   docker/build-host.sh all             # build everything
#   docker/build-host.sh openmw esmtool  # build specific targets
#   docker/build-host.sh clean           # make clean
#   docker/build-host.sh --configure     # re-run cmake only (after CMakeLists changes)
#   docker/build-host.sh --export-libs   # (re)populate native-lib/ only, no build
#   docker/build-host.sh --shell         # interactive shell in the build container
#
# A successful build also refreshes native-lib/ (the container-only shared libs the host
# lacks), so the host-side run scripts (files/mp-run) can launch the container-built binary.
#
# Env overrides:
#   IMAGE=openmw.server:latest   CONTAINER_RUNTIME=podman|docker
#   NPROC=8                      BUILD_DIR=build
set -euo pipefail

# --- repo root (this script lives in <repo>/docker) ---
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

IMAGE="${IMAGE:-openmw.server:latest}"
BUILD_DIR="${BUILD_DIR:-build}"
NPROC="${NPROC:-$(nproc)}"

# --- pick a container runtime: explicit override, else podman (Fedora), else docker ---
runtime="${CONTAINER_RUNTIME:-}"
if [ -z "$runtime" ]; then
    if command -v podman >/dev/null 2>&1; then runtime=podman
    elif command -v docker >/dev/null 2>&1; then runtime=docker
    else echo "error: no podman or docker found" >&2; exit 1; fi
fi
command -v "$runtime" >/dev/null 2>&1 || { echo "error: '$runtime' not found" >&2; exit 1; }

# --- image must exist; if not, tell them how to build it ---
if ! "$runtime" image inspect "$IMAGE" >/dev/null 2>&1; then
    cat >&2 <<EOF
error: image '$IMAGE' not found. Build it once with:
    $runtime build -f "$REPO/docker/Dockerfile.server" -t "$IMAGE" "$REPO/docker"
EOF
    exit 1
fi

# --- parse mode ---
mode=build
case "${1:-}" in
    --configure)   mode=configure ;;
    --export-libs) mode=export-libs ;;
    --shell)       mode=shell ;;
esac

# everything after the first non-flag word = make targets (default: openmw)
targets=()
for a in "$@"; do
    case "$a" in --configure|--export-libs|--shell) ;; *) targets+=("$a") ;; esac
done
[ ${#targets[@]} -eq 0 ] && targets=(openmw)

# --- run flags: :Z relabels the mount for SELinux (Fedora); harmless on WSL/Ubuntu.
# An interactive TTY only when stdin is a terminal (so CI / non-tty callers still work). ---
run_flags=(--rm -v "$REPO:/openmw:Z" -e "NPROC=$NPROC" -w "/openmw/$BUILD_DIR")
[ -t 0 ] && run_flags+=(-it)

# The container runs as root against a host-owned bind mount, so git refuses to touch
# /openmw ("dubious ownership"). Mark it safe so cmake's git version detection works.
safe_git='git config --global --add safe.directory /openmw'

# The image ships Qt6, so the GUI tools (openmw-launcher, openmw-wizard, openmw-cs)
# build by default — no extra cmake flags needed.
cmake_opts=''

# --- export_libs: stage the container-only shared libs into native-lib/ so the host can
# run the container-built binary. Two steps:
#   1) in-container: copy the binary's full dependency closure (minus glibc core + the
#      loader, which the host provides) plus the OSG plugin dir.
#   2) on-host: drop any lib the host's loader already knows (same soname). The GPU/GL,
#      display and C++-runtime stacks MUST come from the host — they talk to the host
#      kernel DRM / glibc — so shadowing them with the Ubuntu copies breaks EGL/rendering.
export_libs() {
    echo "[build] $runtime: exporting native libs -> $REPO/native-lib"
    # The container runs as root; chown the output back to the invoking host user so the
    # host-side prune below (and normal file management) can touch it.
    local hu hg
    hu="$(id -u)"; hg="$(id -g)"
    "$runtime" run "${run_flags[@]}" "$IMAGE" bash -lc '
        set -e
        cd /openmw
        [ -x "'"$BUILD_DIR"'/openmw" ] || { echo "error: '"$BUILD_DIR"'/openmw not built yet" >&2; exit 1; }
        rm -rf native-lib && mkdir native-lib
        ldd "'"$BUILD_DIR"'/openmw" | awk "{print \$3}" | grep -E "^/" | while read -r lib; do
            case "$(basename "$lib")" in
                libc.so.6|libm.so.6|libdl.so.2|libpthread.so.0|librt.so.1|libresolv.so.2|ld-linux*) continue ;;
            esac
            cp -L "$lib" native-lib/
        done
        osgdir=$(dirname "$(ldd "'"$BUILD_DIR"'/openmw" | awk "/libosgDB/ {print \$3}")")
        cp -r "$osgdir"/osgPlugins-* native-lib/ 2>/dev/null || true
        chown -R '"$hu:$hg"' native-lib
    '
    # Prune anything the host already provides. The dynamic linker always searches the
    # standard lib dirs, so a matching soname file there means the host satisfies it —
    # keep only the truly-missing (container-only) libs.
    local pruned=0 path f
    for path in "$REPO"/native-lib/*.so*; do
        [ -e "$path" ] || continue
        f="$(basename "$path")"
        if [ -e "/usr/lib64/$f" ] || [ -e "/lib64/$f" ] || [ -e "/usr/lib/$f" ]; then
            rm -f "$path"; pruned=$((pruned + 1))
        fi
    done
    echo "[build] native-lib: $(ls "$REPO"/native-lib/*.so* 2>/dev/null | wc -l) container libs kept, $pruned host-provided pruned"
}

case "$mode" in
    shell)
        echo "[build] $runtime shell in $IMAGE  (cwd=/openmw/$BUILD_DIR)"
        exec "$runtime" run "${run_flags[@]}" "$IMAGE" bash
        ;;
    configure)
        echo "[build] $runtime: cmake reconfigure"
        exec "$runtime" run "${run_flags[@]}" "$IMAGE" bash -lc "$safe_git; cmake .. $cmake_opts"
        ;;
    export-libs)
        export_libs
        ;;
    build)
        echo "[build] $runtime: make ${targets[*]} -j$NPROC  (image=$IMAGE)"
        "$runtime" run "${run_flags[@]}" "$IMAGE" \
            bash -lc "$safe_git; cmake .. $cmake_opts >/dev/null && make ${targets[*]} -j\"$NPROC\""
        # Refresh the host-side run libs to match the binary we just built.
        export_libs
        ;;
esac
