#!/bin/bash
# =============================================================================
# ANARI-USD benchmark run — JURECA-DC (TEMPLATE, adjust to your allocation)
#
# Usage: sbatch sbatch_benchmark.sh <particles_M> <mode>
#   <particles_M> : 2 | 5 | 10 | 20 | 40 | 80   (millions of particles)
#   <mode>        : memory | disk
#
# memory : usd::enableSaving=false (default build) — in-memory + ZMQ path
# disk   : usd::enableSaving=true  — default UsdStage::Save to parallel FS
#
# Both simulation and pvserver run in one allocation; the paper's evaluated
# split was 52 sim nodes / 8 pvserver nodes on dc-cpu. Adjust NNODES_SIM /
# NNODES_PVSERVER, partition, QoS and account to your allocation.
# =============================================================================
#SBATCH --job-name=anari_usd_bench
#SBATCH --partition=dc-cpu            # TODO: adjust
#SBATCH --qos=<qos>                   # TODO: adjust
#SBATCH --account=<project>           # TODO: adjust
#SBATCH --nodes=60
#SBATCH --ntasks-per-node=<cores>     # TODO: adjust
#SBATCH --time=04:00:00
#SBATCH --output=bench_%j.out

SIZE_M="$1"
MODE="$2"

if [ -z "$SIZE_M" ] || [ -z "$MODE" ]; then
    echo "Usage: sbatch sbatch_benchmark.sh <2|5|10|20|40|80> <memory|disk>"
    exit 1
fi

# ---- benchmark report location (shared by sim + pvserver ranks) -------------
export ANARI_USD_BENCHMARK_OUT=$SCRATCH/$USER/anari_usd_bench/p${SIZE_M}M_${MODE}
mkdir -p "$ANARI_USD_BENCHMARK_OUT"

# ---- ANARI-USD device environment -------------------------------------------
# TODO: point at your built ANARI-USD (ZMQ-enabled) on the login/scratch path
export ANARI_LIBRARY='usd'
export ANARI_USD_SERIALIZE_LOCATION=$SCRATCH/$USER/anari_usd_bench/p${SIZE_M}M_${MODE}/usd_out
mkdir -p "$ANARI_USD_SERIALIZE_LOCATION"
export LD_LIBRARY_PATH=/p/home/jusers/$USER/jureca/parsimcat_simulation_setup/ANARI-USD/build:$LD_LIBRARY_PATH

# disk mode: the device must be told to use the default save path.
# The ANARI parameter is usd::enableSaving (default false = memory path).
# If your pvserver Catalyst/python setup exposes ANARI device parameters, set
# usd::enableSaving=true here for disk runs; otherwise set it in the Catalyst
# render settings. memory runs need nothing (default).
if [ "$MODE" = "disk" ]; then
    echo "[bench] disk mode: ensure usd::enableSaving=true in the ANARI render settings"
fi

echo "============================================================"
echo " ANARI-USD benchmark: ${SIZE_M}M particles, mode=${MODE}"
echo " reports -> ${ANARI_USD_BENCHMARK_OUT}"
echo "============================================================"

# ---- launch ------------------------------------------------------------------
# TODO: replace TODO_* placeholders with your actual sim + pvserver launch for
# the FEL pipeline. The paper's deployment: 52 nodes running the IPPL FEL
# mini-app with Catalyst, 8 nodes running pvserver (MPI) with the ANARI/USD
# backend.

NNODES_SIM=52
NNODES_PVSERVER=8

# Simulation (Catalyst pushes particle data in-situ to pvserver)
srun --nodes=$NNODES_SIM --ntasks-per-node=$SLURM_NTASKS_PER_NODE \
    TODO_ippl_fel_runner --particles $((SIZE_M * 1000000)) \
    --catalyst TODO_catalyst_config \
    || exit 1

# pvserver (ANARI-USD backend; rank 0 hosts the ZMQ broker on 5555/5556)
# TODO: match your pvserver invocation (Catalyst client, ANARI env above)
srun --nodes=$NNODES_PVSERVER --ntasks-per-node=$SLURM_NTASKS_PER_NODE \
    pvsbatch pvserver TODO_pvserver_args \
    || exit 1

# When the pvserver ranks exit, each ~UsdDevice() writes
# benchmark_rank_<rank>.json/.csv and rank 0's broker writes
# benchmark_broker.json into $ANARI_USD_BENCHMARK_OUT.

echo "[bench] done: ${SIZE_M}M/${MODE} — reports in ${ANARI_USD_BENCHMARK_OUT}"
ls -la "$ANARI_USD_BENCHMARK_OUT"
