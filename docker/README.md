# OpenMW multiplayer — containerized build & run

Build OpenMW and run a dedicated multiplayer server + rendering clients, all inside a container,
so you don't need a host toolchain. Works with **podman** (Fedora, rootless) or **docker**
(auto-detected; override with `CONTAINER_RUNTIME=podman|docker`).

## Prerequisites

- podman or docker.
- A Morrowind install. The launchers read the `data=` directories from your `openmw.cfg` and mount
  them, so wherever the game lives (native Steam, Flatpak, GOG, manual) it works without configuration.
  Override with `OPENMW_DATA=<Data Files path>` if you want a specific one.
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

The client takes no arguments beyond the server address. Everything else happens in-game on
connect: the server offers its stored characters — pick one to resume it, or choose
**New character** to create one through the normal character-creation intro (prison ship, census
office) and join the shared world from there.

Host a specific save instead of a new world — point the **server** at a host `.omwsave` file:

```sh
docker/run-server.sh --save ~/openmw-mp-userdata/saves/test/MP.omwsave   # server hosts the save
docker/run-client.sh <IP>                                                # clients just connect
```

The save is mounted read-only. Clients never load a save themselves; the server serves each client
its character over the wire. (`OPENMW_SAVE=<path>` is the env equivalent of `--save`.)

## Scripts

| Script | What it does |
|---|---|
| `build-host.sh` | Build the image (once) and compile openmw incrementally. Also `all`, `clean`, specific targets, `--configure`, `--shell`. |
| `run-server.sh` | Headless dedicated server (software GL, no window). `--listen PORT`, plus any openmw args. |
| `run-client.sh` | Rendering client (`<SERVER_IP[:PORT]>`). Auto-detects GPU; character select / creation happens in-game on connect. |
| `_mp-common.sh` | Shared helpers sourced by the run scripts (not run directly). |
| `Dockerfile.server` | The Ubuntu-based build/run image (OpenSceneGraph, Bullet, MyGUI 3.4.3 from source, Recast, …). |

## Environment overrides

All host-specific paths default to this machine but can be overridden:

| Var | Default | Meaning |
|---|---|---|
| `OPENMW_DATA` | _(read from openmw.cfg)_ | force a specific Morrowind `Data Files` path |
| `OPENMW_CONFIG` | `~/.config/openmw` | openmw config dir (copied per instance) |
| `OPENMW_USERDATA` | client: throwaway temp dir; server: `~/openmw-mp-server-data` | dir mounted at `/userdata` for saves. Clients get a wiped copy each run; the **server's is persistent** (bind-mounted read-write) so `SIGUSR1` saves survive restarts |
| `OPENMW_SAVE` | _(none)_ | server only: a host `.omwsave` to host (same as `--save`) |
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
