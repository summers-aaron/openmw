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
#   docker/build-host.sh --shell         # interactive shell in the build container
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

# --- image must exist; build it once (first run) if it doesn't ---
if ! "$runtime" image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "[build] image '$IMAGE' not found — building it once with $runtime (this takes a while)..."
    "$runtime" build -f "$REPO/docker/Dockerfile.server" -t "$IMAGE" "$REPO/docker"
fi

# --- parse mode ---
mode=build
case "${1:-}" in
    --configure) mode=configure ;;
    --shell)     mode=shell ;;
esac

# everything after the first non-flag word = make targets (default: openmw)
targets=()
for a in "$@"; do
    case "$a" in --configure|--shell) ;; *) targets+=("$a") ;; esac
done
[ ${#targets[@]} -eq 0 ] && targets=(openmw)

# --- run flags: :Z relabels the mount for SELinux (Fedora); harmless on WSL/Ubuntu.
# An interactive TTY only when stdin is a terminal (so CI / non-tty callers still work). ---
run_flags=(--rm -v "$REPO:/openmw:Z" -e "NPROC=$NPROC" -w "/openmw/$BUILD_DIR")
[ -t 0 ] && run_flags+=(-it)

case "$mode" in
    shell)
        echo "[build] $runtime shell in $IMAGE  (cwd=/openmw/$BUILD_DIR)"
        exec "$runtime" run "${run_flags[@]}" "$IMAGE" bash
        ;;
    configure)
        echo "[build] $runtime: cmake reconfigure"
        exec "$runtime" run "${run_flags[@]}" "$IMAGE" bash -lc 'cmake ..'
        ;;
    build)
        echo "[build] $runtime: make ${targets[*]} -j$NPROC  (image=$IMAGE)"
        exec "$runtime" run "${run_flags[@]}" "$IMAGE" \
            bash -lc "cmake .. >/dev/null && make ${targets[*]} -j\"$NPROC\""
        ;;
esac
