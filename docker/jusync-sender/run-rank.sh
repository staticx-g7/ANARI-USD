#!/bin/bash
# Per-rank launch wrapper for run-sender-mpi.sh: rank-local serialize dir so
# ranks never race on the same on-disk assets, then exec the sender binary.
# mpirun has already exported OMPI_COMM_WORLD_RANK which the UsdDevice uses
# for rank wiring (do NOT fake these vars here, unlike run-sender.sh).
set -e
BIN="$1"
shift || true
R="${OMPI_COMM_WORLD_RANK:-0}"
export ANARI_USD_SERIALIZE_LOCATION="/work/rank$R"
mkdir -p "$ANARI_USD_SERIALIZE_LOCATION"
cd "$ANARI_USD_SERIALIZE_LOCATION"
exec "$BIN" "$@"
