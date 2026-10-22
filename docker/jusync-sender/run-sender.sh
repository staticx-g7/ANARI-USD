#!/bin/bash
# Run an ANARI->USD streaming sender with the JUSYNC ZMQ broker exposed on
# tcp://0.0.0.0:5556 for the client (the UE host on Windows connects through
# `docker -p 5556:5556` / localhost, standing in for the SSH tunnel to JURECA).
#
# usage: run-sender.sh [binary] [args...]
#   binary default: anariTutorialUsdTime (per-timestep commits -> live updates)
#   (installed example names are camelCase from examples/CMakeLists.txt
#    project() targets, e.g. anariTutorialUsd / ...Time / ...Recreate /
#    ...Volume / ...DualWorld — NOT the .c file names)
#
# env:
#   JUSYNC_SENDER_BIN     sender executable (overridden by $1)
#   JUSYNC_LOOP=1         re-run forever (continuous clip updates like a sim)
#   JUSYNC_LOOP_DELAY=2   seconds between loop iterations
#   JUSYNC_RAWGEO_STRIDE  RawGeo point subsampling for this sender (default 32)
#   ANARI_USD_SERIALIZE_LOCATION  where disk-mode assets go (default /work)
set -e

BIN="${1:-${JUSYNC_SENDER_BIN:-anariTutorialUsdTime}}"
shift || true

# The UsdDevice starts the ZMQ broker when an MPI rank environment is present
# (single rank -> rank-0 broker path, no real MPI launch needed; see UsdDevice
# init: OMPI_COMM_WORLD_* / SLURM_* / PMI_* detection).
export OMPI_COMM_WORLD_RANK=0
export OMPI_COMM_WORLD_SIZE=1

export ANARI_USD_SERIALIZE_LOCATION="${ANARI_USD_SERIALIZE_LOCATION:-/work}"
mkdir -p "$ANARI_USD_SERIALIZE_LOCATION"
cd "$ANARI_USD_SERIALIZE_LOCATION"

# RawGeo decimation belongs here, beside the multi-million-point clouds it was
# tuned for. In image ENV it also lands on interactive ParaView sessions, whose
# launchers then cannot override it (a baked value always beats ${VAR:-default}).
export JUSYNC_RAWGEO_STRIDE="${JUSYNC_RAWGEO_STRIDE:-32}"

echo "[run-sender] broker tcp://0.0.0.0:5556 (JUSYNC client) / 5555 (ranks)"
echo "[run-sender] starting: $BIN $*"

if [ "${JUSYNC_LOOP:-0}" = "1" ]; then
    i=0
    while true; do
        i=$((i+1))
        echo "[run-sender] === pass $i ==="
        "$BIN" "$@" || echo "[run-sender] $BIN exited $? (loop mode continues)"
        sleep "${JUSYNC_LOOP_DELAY:-2}"
    done
else
    exec "$BIN" "$@"
fi
