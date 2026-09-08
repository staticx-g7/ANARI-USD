// Copyright 2024 ANARI-USD Contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef USD_BRIDGE_BENCHMARK_H
#define USD_BRIDGE_BENCHMARK_H

#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <chrono>
#include <cstdint>
#include <cstddef>

namespace usd_bridge_benchmark {

// One recorded USD stage commit. Memory mode (in-memory serialization + ZMQ
// streaming) and disk mode (default UsdStage::Save) both land here so a single
// report file per rank captures whichever path the device was run with.
struct CommitRecord {
    long long seq = 0;         // per-rank commit counter
    double t = 0.0;            // epoch seconds
    std::string stage;         // stage name (or "name (recalc)" / "name (disk)")
    bool diskMode = false;     // true: UsdStage::Save path, false: memory path
    size_t rawBytes = 0;       // serialized USD size (pre-zstd) / saved file size
    size_t wireBytes = 0;      // on-the-wire size after zstd (memory mode only)
    double serializeMs = 0.0;  // ExportToString / usdc WriteToString (memory mode)
    double storeMs = 0.0;      // zstd+hash+store (memory) or UsdStage::Save (disk)
};

// Per-rank benchmark recorder for the ANARI-USD device.
//
// Enabled by the environment variable ANARI_USD_BENCHMARK_OUT=<dir>; when unset
// every call is a no-op (single static bool check), so the shipping path is
// untouched. While enabled, every TrackStageMemory commit and every disk-mode
// Save() is recorded with wall-clock stage timings and byte counts.
//
// WriteReports() emits, per rank:
//   <dir>/benchmark_rank_<rank>.json  - full report (totals, per-stage stats, records)
//   <dir>/benchmark_rank_<rank>.csv   - one row per commit, for tooling
class UsdBridgeBenchmark {
public:
    static UsdBridgeBenchmark& Instance();

    bool Enabled() const { return enabled_; }
    int Rank() const { return rank_; }

    // Call once at device setup. outDir empty -> disabled.
    void Init(int rank, const std::string& outDir);

    void RecordMemoryCommit(const std::string& stage, size_t rawBytes, size_t wireBytes,
                            double serializeMs, double storeMs);
    void RecordDiskSave(const std::string& stage, size_t bytes, double saveMs);

    // Cumulative bytes/chunks/files this rank actually sent to clients (worker
    // file-serving loop). Recorded once, right before WriteReports().
    void AddServingStats(uint64_t bytes, uint64_t chunks, uint64_t files);

    // Write JSON + CSV reports. Safe to call repeatedly; only the first call writes.
    void WriteReports();

    // Compact JSON summary of the current state (totals, per-commit stats,
    // serving counters) - no records[] array. Served live to clients that
    // request the synthetic "__benchmark_rank__.json" file so each JuSync run
    // can record the cluster side without waiting for a report file.
    std::string SummaryJson() const;

    size_t CommitCount() const;
    size_t TotalRawBytes() const;
    size_t TotalWireBytes() const;
    size_t TotalDiskBytes() const;
    double TotalSerializeMs() const;
    double TotalStoreMs() const;

private:
    UsdBridgeBenchmark() = default;

    mutable std::mutex mutex_;
    bool enabled_ = false;
    bool written_ = false;
    int rank_ = -1;
    std::string outDir_;
    long long seq_ = 0;
    double startEpoch_ = 0.0;
    uint64_t servedBytes_ = 0;
    uint64_t servedChunks_ = 0;
    uint64_t servedFiles_ = 0;
    std::vector<CommitRecord> records_;
};

// Time a UsdStage::Save() call site and record it. Replaces a plain
// `stage->Save();` in disk mode:
//
//   usd_bridge_benchmark::TimedDiskSave(
//       [&]{ geomStage->Save(); }, "geom",
//       geomStage->GetRootLayer()->GetIdentifier());
//
// `filePath` is optional; when non-empty the saved file size is captured via stat.
// Returns the measured save time in milliseconds. No-op timing overhead is one
// steady_clock::now() pair when the benchmark is disabled.
double TimedDiskSave(const std::function<void()>& save, const std::string& name,
                     const std::string& filePath = "");

} // namespace usd_bridge_benchmark

#endif // USD_BRIDGE_BENCHMARK_H
