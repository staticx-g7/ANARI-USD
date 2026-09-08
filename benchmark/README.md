# ANARI-USD Cluster-Side Benchmark

Instrumentation that captures, per MPI rank, everything the paper's reviewers
asked for on the **cluster side** of the pipeline:

| Metric | Reviewer ask | Where recorded |
|---|---|---|
| USD serialization time per stage commit (ExportToString / usdc) | R2: "provide the serialization measurements listed in Section V-A" | `serialize_ms` in `TrackStageMemory` / `RecalculateAllMemoryUsage` |
| zstd+hash+store time per commit | R2 (above) | `store_ms` |
| Raw USD bytes per commit (pre-compression) | R1: "total USD payload size and total bytes per rank" | `raw_bytes` |
| On-the-wire bytes per commit (post-zstd) | R1 (above) | `wire_bytes` |
| **Bytes/chunks/files this rank actually served to clients** | R1: "number of chunks sent to rendering clients" | `serving` section (worker file-serving loop counters) |
| Disk-mode `UsdStage::Save()` wall time + file size per stage | R2/R4: "compare the modified path with the default disk-writing ANARI-USD workflow" | `disk_mode` records via `TimedDiskSave` |
| Broker: total bytes relayed to all clients, message count, workers, duration | R1/R2 (broker delivery characterization) | `benchmark_broker.json` (rank 0) |

## Enabling

Set the environment variable **before launching pvserver** (any rank process):

```bash
export ANARI_USD_BENCHMARK_OUT=$SCRATCH/$USER/anari_usd_bench/<run_name>
mkdir -p "$ANARI_USD_BENCHMARK_OUT"
```

When unset, every measurement is a single atomic/static check — no behavioral
or runtime change to the shipping path.

## Modes

The device's `usd::enableSaving` parameter selects the data path
(default in the current build: `false` = memory path):

- **memory** (`usd::enableSaving=false`): commits go through
  `TrackStageMemory` → memory store → ZMQ. Records:
  `serialize_ms`, `store_ms`, `raw_bytes`, `wire_bytes` + serving counters.
- **disk** (`usd::enableSaving=true`): commits go through `UsdStage::Save()`
  into `ANARI_USD_SERIALIZE_LOCATION` (parallel storage). Records:
  `store_ms` (Save wall time), `raw_bytes` (saved file size), named by stage
  (`geom`, `material`, `sampler`, `light`, `camera`, `scene`, ...).

Run the same size matrix in both modes for the disk-vs-memory comparison.

## Outputs

Per rank (written at device destruction, i.e. end of the pvserver process):

```
$ANARI_USD_BENCHMARK_OUT/benchmark_rank_<rank>.json   # full report
$ANARI_USD_BENCHMARK_OUT/benchmark_rank_<rank>.csv    # one row per commit
```

Broker (rank 0, at broker shutdown):

```
$ANARI_USD_BENCHMARK_OUT/benchmark_broker.json
```

JSON schema (per rank):

```json
{
  "rank": 3,
  "mode": "memory",
  "commit_count": 340,
  "duration_s": 298.1,
  "serving": { "bytes": 596827359, "chunks": 142, "files": 340 },
  "totals": {
    "raw_bytes": 596827359, "wire_bytes": 596827359,
    "disk_bytes": 0,
    "serialize_ms_total": 4123.5, "store_ms_total": 876.2
  },
  "per_commit": {
    "serialize_ms": {"min": 0.4, "median": 8.2, "max": 214.9},
    "store_ms":     {"min": 0.1, "median": 1.9, "max": 45.0},
    "raw_bytes":    {"min": 1024, "median": 170567, "max": 902331},
    "wire_bytes":   {"min": 1024, "median": 170567, "max": 902331}
  },
  "records": [ { "seq": 0, "t": 1788780000.0, "stage": "geom",
                 "disk_mode": false, "raw_bytes": 170567,
                 "wire_bytes": 170567, "serialize_ms": 8.2, "store_ms": 1.9 }, ... ]
}
```

Notes:
- `serving.bytes` is what the rank actually transmitted (the client's
  `BytesDownloaded` is the same quantity summed over ranks, minus any
  rank-0-local relay overhead — the two should agree closely and make a good
  cross-check).
- In memory mode `wire_bytes` ≤ `raw_bytes` (zstd level 1); for binary
  `.usdc` content compression may be neutral, in which case raw == wire.
- The `records` array can be large for long runs; delete it from a copy if
  you only need the aggregates for the paper tables.

## Running the size matrix

Six dataset sizes (2/5/10/20/40/80 M particles) × two modes = 12 cluster runs,
each paired with one client run (the JUSYNC pipeline harness on the UE side,
which already writes `*millionFEL.csv/json`).

```bash
# one run:
sbatch sbatch_benchmark.sh 20 memory    # 20 M particles, memory path
sbatch sbatch_benchmark.sh 20 disk      # same size, default disk path
```

After all runs, gather and summarize:

```bash
./collect_reports.sh $SCRATCH/$USER/anari_usd_bench/<run_name> results/
python3 ../../<paper-repo>/Benchmark/merge_reports.py results/ \
    <paper-repo>/Benchmark/   # merges cluster JSON + client CSV into paper tables
```

## Caveats (state these in the paper)

- Serialization/store times are per-stage wall times on the pvserver ranks;
  the simulation keeps running concurrently, so they are not isolated
  microbenchmarks — they are the in-situ cost of the data path.
- Disk-mode `Save()` writes go to the parallel filesystem; wall time includes
  page-cache and Lustre latency, which is precisely the contention the
  memory path removes.
- Serving counters count bytes placed onto the ZMQ socket, not bytes
  acknowledged by the client.
