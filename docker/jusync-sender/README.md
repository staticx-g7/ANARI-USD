# JUSYNC streaming sender rig

Self-contained Linux image that builds and runs this repo's **ANARI USD device +
UsdBridge ZMQ broker**, plus a VNC/noVNC desktop with ParaView — so the whole
`ANARI -> USD clips -> broker` path can be exercised locally on Windows, with
the UE JUSYNC plugin connecting to it exactly like it does to JURECA over the
SSH tunnel (here just `docker -p 5556:5556` -> `tcp://localhost:5556`).

## Why this exists

- Phase 1 of the realtime-streaming plan: validate the **binary crate
  (`usd::serialize.outputBinary=true`)** path end to end without JURECA batch
  jobs. The examples in `examples/` now default to binary output.
- The device itself is CPU-only (it records ANARI calls to USD); the GPU is
  optional — used for `--gpus all` render paths (scivis/helium) and ParaView
  hardware GL. Without a GPU everything still works (llvmpipe).

## Build

```powershell
# from the repo root (or anywhere):
.\docker\jusync-sender\build.ps1          # ~30-90 min first time (ANARI SDK + OpenUSD 25.11)
```
or raw:
```powershell
docker build -f docker/jusync-sender/Dockerfile -t jusync-sender:dev .
```

What the image builds on **ubuntu:24.04** (noble — its oneTBB 2021.12 is what
OpenUSD 25.11's work backend needs; jammy's 2021.5 fails to compile against it):
Khronos **ANARI-SDK** (`main`, same ref as the repo superbuild), **OpenUSD
v25.11** core-only (boost-free, no imaging/python), and this repo with
`-DUSD_DEVICE_BUILD_EXAMPLES=ON`.

## Run

```powershell
.\docker\jusync-sender\run.ps1            # GPU if available, CPU fallback; loops sender
.\docker\jusync-sender\run.ps1 -OneShot   # single commit pass
docker compose -f docker/jusync-sender/docker-compose.yml up --build
```

Endpoints:

| port | what |
|------|------|
| 5556 | JUSYNC ZMQ broker (client) — point the UE spawner at `tcp://localhost:5556` |
| 6080 | noVNC web desktop (`http://localhost:6080/vnc.html`, password `jusync`) |
| 5901 | raw VNC (TigerVNC, use any VNC client) |
| 5555 | inter-rank worker port (solo run: unused) |

Env knobs: `JUSYNC_SENDER_BIN` (default `anariTutorialUsdTime` — installed
example binaries are camelCase, see `examples/CMakeLists.txt`), `JUSYNC_LOOP=1`
(re-run forever, sim-like continuous clip updates), `JUSYNC_LOOP_DELAY`,
`JUSYNC_KEEPUP=1` (keep container alive after command exit for `docker exec -it
jusync-sender bash`), `ANARI_USD_SERIALIZE_LOCATION` (default `/work`),
`JUSYNC_VNC_RESOLUTION` (default `1600x900x24`).

The broker start is triggered via the single-rank MPI env path: `run-sender.sh`
exports `OMPI_COMM_WORLD_RANK=0 SIZE=1`, which makes `UsdDevice` init take the
rank-0 broker branch without an actual MPI launcher.

## E2E with the JUSYNC UE plugin (Phase 1 test)

1. Build/run the UE sample with a middleware whose host/endpoint is
   `tcp://localhost:5556` (replaces the JURECA SSH-tunnel endpoint).
2. `run.ps1` starts the looping `anariTutorialUsdTime` sender — it commits
   time-varying geometry per timestep, so every ~2 s a clip update is notified.
3. Watch `jusync.diag` in the UE console: per-file rows should show transfer +
   parse of the binary crates (files keep their historical `.usda` store keys;
   the client sniffs the `PXR-USDC` magic after zstd).
4. Open noVNC + ParaView in the container if you want to eyeball the same
   dataset that the broker is serving (`/work`).

## Files

- `Dockerfile` — multi-stage builder + runtime (VNC/noVNC/ParaView).
- `entrypoint.sh` — VNC desktop + noVNC, then runs the CMD.
- `run-sender.sh` — broker env + loop mode around any ANARI->USD example.
- `build.ps1` / `run.ps1` — Windows conveniences (GPU autodetect w/ fallback).
- `docker-compose.yml` — same, GPU reservation via nvidia container toolkit.
