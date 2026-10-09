#!/bin/bash
# Multi-rank sender rig: launches the big-cloud example under mpirun so rank 0
# runs the ZMQ broker and ranks >= 1 connect as ZmqWorkers (same code path as
# JURECA, one container). Replaces run-sender.sh when JUSYNC_MPI_NPROCS > 1.
set -e
N="${JUSYNC_MPI_NPROCS:-2}"
BIN="${JUSYNC_SENDER_BIN:-/opt/sender/anariBigCloudUsd}"

# Single-container OpenMPI: shared-memory transport, no rdma/hw components.
export OMPI_MCA_btl=self,vader,tcp
export OMPI_MCA_btl_vader_single_copy_mechanism=none

echo "[run-sender-mpi] mpirun -n $N $BIN"
exec mpirun --allow-run-as-root --oversubscribe -n "$N" /bin/sh /opt/sender/run-rank.sh "$BIN"
