#!/bin/bash
# Gather per-rank + broker benchmark reports from a run directory into a
# results folder, validate the JSON, and print a one-line summary per rank.
#
# Usage: ./collect_reports.sh <ANARI_USD_BENCHMARK_OUT dir> <results dir>

set -euo pipefail

SRC="${1:?usage: collect_reports.sh <bench_out_dir> <results_dir>}"
DST="${2:?usage: collect_reports.sh <bench_out_dir> <results_dir>}"

mkdir -p "$DST"

shopt -s nullglob
files=( "$SRC"/benchmark_rank_*.json )
if [ -f "$SRC/benchmark_broker.json" ]; then
    files+=( "$SRC/benchmark_broker.json" )
fi
if [ ${#files[@]} -eq 0 ]; then
    echo "No benchmark reports found in $SRC" >&2
    exit 1
fi

cp "${files[@]}" "$DST/"

python3 - "$DST" <<'EOF'
import glob, json, os, sys

dst = sys.argv[1]
rank_files = sorted(glob.glob(os.path.join(dst, "benchmark_rank_*.json")),
                    key=lambda p: int(os.path.basename(p).split("_")[-1].split(".")[0]))
print(f"{'rank':>5} {'mode':>7} {'commits':>8} {'raw MB':>10} {'wire MB':>10} {'serv MB':>10} {'serv ch':>8} {'serialize s':>12} {'store s':>9}")
for f in rank_files:
    d = json.load(open(f))  # raises if invalid
    t = d["totals"]; s = d.get("serving", {})
    print(f"{d['rank']:>5} {d['mode']:>7} {d['commit_count']:>8} "
          f"{t['raw_bytes']/1048576:>10.1f} {t['wire_bytes']/1048576:>10.1f} "
          f"{s.get('bytes',0)/1048576:>10.1f} {s.get('chunks',0):>8} "
          f"{t['serialize_ms_total']/1000:>12.2f} {t['store_ms_total']/1000:>9.2f}")

broker = os.path.join(dst, "benchmark_broker.json")
if os.path.exists(broker):
    b = json.load(open(broker))
    r = b["relayed_to_clients"]
    print(f"\nbroker (rank 0): workers={b['workers']} duration={b['duration_s']:.1f}s "
          f"relayed={r['bytes']/1048576:.1f} MB in {r['messages']} messages")
print(f"\nvalid JSON: {len(rank_files)} rank reports (+broker)" if os.path.exists(broker)
      else f"\nvalid JSON: {len(rank_files)} rank reports (no broker report)")
EOF

echo
echo "Copied to $DST"
