# OpenMW multiplayer — containerized build & run

Build OpenMW and run a dedicated multiplayer server + rendering clients, all inside a container,
so you don't need a host toolchain. Works with **podman** (Fedora, rootless) or **docker**
(auto-detected; override with `CONTAINER_RUNTIME=podman|docker`).

## Prerequisites

- podman or docker.
- A Morrowind install (the **`Data Files`** directory). Its path must match the `data=` line in your
  `openmw.cfg`, because the container mounts it at that same path.
- An `openmw.cfg` (default `~/.config/openmw`) listing your content (`Morrowind.esm`, …).

## Quick start

```sh
docker/build-host.sh                 # builds the build image (first run only), then compiles openmw
docker/run-server.sh                 # terminal A: a headless dedicated server on :25565
docker/run-client.sh 127.0.0.1       # terminal B: a rendering client that joins it
```

Use a LAN IP instead of `127.0.0.1` to join a server on another machine. Two players on one box —
just run it twice; each gets its own container name (`openmw-client`, `openmw-client-2`) and coexists:

```sh
docker/run-client.sh <IP>     # first client
docker/run-client.sh <IP>     # second client (auto-named, runs alongside the first)
```

Set `NAME=…` only if you want a specific, fixed container name (re-running with the same `NAME`
replaces that one).

Host a specific save instead of a new game:

```sh
OPENMW_USERDATA=~/openmw-mp-userdata docker/run-server.sh --load-savegame /userdata/saves/test/MP.omwsave
```

(`OPENMW_USERDATA` is copied to `/userdata` inside the container; the original is never modified.)

## Scripts

| Script | What it does |
|---|---|
| `build-host.sh` | Build the image (once) and compile openmw incrementally. Also `all`, `clean`, specific targets, `--configure`, `--shell`. |
| `run-server.sh` | Headless dedicated server (software GL, no window). `--listen PORT`, plus any openmw args. |
| `run-client.sh` | Rendering client (`<SERVER_IP[:PORT]>`). Auto-detects GPU, starts a new game unless you pass `--load-savegame`. |
| `_mp-common.sh` | Shared helpers sourced by the run scripts (not run directly). |
| `Dockerfile.server` | The Ubuntu-based build/run image (OpenSceneGraph, Bullet, MyGUI 3.4.3 from source, Recast, …). |

## Environment overrides

All host-specific paths default to this machine but can be overridden:

| Var | Default | Meaning |
|---|---|---|
| `OPENMW_DATA` | a Steam Morrowind path | Morrowind `Data Files` (must match `openmw.cfg` `data=`) |
| `OPENMW_CONFIG` | `~/.config/openmw` | openmw config dir (copied per instance) |
| `OPENMW_USERDATA` | throwaway temp dir | dir mounted at `/userdata` for saves (copied) |
| `IMAGE` | `openmw.server:latest` | build/run image tag |
| `CONTAINER_RUNTIME` | auto | `podman` or `docker` |
| `PORT` | `25565` | default port when none is in the address |
| `GPU` | `auto` | client GPU: `auto` \| `mesa` \| `nvidia` |
| `NAME` | per-script | container name (use distinct names for multiple clients) |
| `NPROC` | all cores | build parallelism (`build-host.sh`) |

## Notes

- The client renders a real window via your X11/XWayland display. On NVIDIA the host's NVIDIA GL/GLX
  userspace is staged into the (Mesa-only) image automatically; on Intel/AMD it uses `/dev/dri`.
  If no window appears, run `xhost +local:` and retry.
- The server listens on all interfaces, so LAN clients can connect via the host's IP. Open the port
  in your firewall if needed.
- The build cache under `build/` is configured for the in-container source path (`/openmw`), so build
  only through `build-host.sh`, not a host `make`.

## Legacy generic build image

`Dockerfile.ubuntu` + `build.sh` are the original distro-agnostic OpenMW build image (unrelated to the
multiplayer harness above):

```sh
docker build -f Dockerfile.ubuntu -t openmw.ubuntu .
docker run -v /path/to/openmw:/openmw:Z -e NPROC=2 -it openmw.ubuntu
```
