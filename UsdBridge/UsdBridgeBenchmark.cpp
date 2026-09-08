// Copyright 2024 ANARI-USD Contributors
// SPDX-License-Identifier: Apache-2.0

#include "UsdBridgeBenchmark.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>

namespace usd_bridge_benchmark {

UsdBridgeBenchmark& UsdBridgeBenchmark::Instance()
{
    static UsdBridgeBenchmark instance;
    return instance;
}

void UsdBridgeBenchmark::Init(int rank, const std::string& outDir)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled_) return;
    if (outDir.empty()) return;

    enabled_ = true;
    rank_ = rank;
    outDir_ = outDir;
    startEpoch_ = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    seq_ = 0;

    // Best-effort directory creation (parent must exist, as with the serialize
    // location the device already relies on).
    ::mkdir(outDir_.c_str(), 0755);

    std::cout << "[Benchmark] Rank " << rank << ": recording enabled, reports to '"
              << outDir_ << "' (benchmark_rank_" << rank << ".json/.csv)" << std::endl;
}

void UsdBridgeBenchmark::RecordMemoryCommit(const std::string& stage, size_t rawBytes,
                                            size_t wireBytes, double serializeMs, double storeMs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) return;

    CommitRecord rec;
    rec.seq = seq_++;
    rec.t = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    rec.stage = stage;
    rec.diskMode = false;
    rec.rawBytes = rawBytes;
    rec.wireBytes = wireBytes;
    rec.serializeMs = serializeMs;
    rec.storeMs = storeMs;
    records_.push_back(std::move(rec));
}

void UsdBridgeBenchmark::RecordDiskSave(const std::string& stage, size_t bytes, double saveMs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) return;

    CommitRecord rec;
    rec.seq = seq_++;
    rec.t = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    rec.stage = stage;
    rec.diskMode = true;
    rec.rawBytes = bytes;
    rec.wireBytes = 0;
    rec.serializeMs = 0.0;
    rec.storeMs = saveMs;
    records_.push_back(std::move(rec));
}

size_t UsdBridgeBenchmark::CommitCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.size();
}

size_t UsdBridgeBenchmark::TotalRawBytes() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = 0;
    for (const auto& r : records_) total += r.rawBytes;
    return total;
}

size_t UsdBridgeBenchmark::TotalWireBytes() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = 0;
    for (const auto& r : records_) total += r.wireBytes;
    return total;
}

size_t UsdBridgeBenchmark::TotalDiskBytes() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = 0;
    for (const auto& r : records_) if (r.diskMode) total += r.rawBytes;
    return total;
}

double UsdBridgeBenchmark::TotalSerializeMs() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    double total = 0.0;
    for (const auto& r : records_) total += r.serializeMs;
    return total;
}

double UsdBridgeBenchmark::TotalStoreMs() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    double total = 0.0;
    for (const auto& r : records_) total += r.storeMs;
    return total;
}

namespace {

struct TripleStats { double min; double median; double max; };

TripleStats StatsOf(std::vector<double> v)
{
    TripleStats s{0.0, 0.0, 0.0};
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    s.min = v.front();
    s.max = v.back();
    s.median = v[v.size() / 2];
    return s;
}

std::string FormatTriple(const TripleStats& s)
{
    std::ostringstream oss;
    oss << "{\"min\":" << s.min << ",\"median\":" << s.median << ",\"max\":" << s.max << "}";
    return oss.str();
}

// Escape a string for embedding in JSON (stage names are simple, but be safe).
std::string JsonEscape(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

} // namespace

void UsdBridgeBenchmark::AddServingStats(uint64_t bytes, uint64_t chunks, uint64_t files)
{
    std::lock_guard<std::mutex> lock(mutex_);
    servedBytes_ = bytes;
    servedChunks_ = chunks;
    servedFiles_ = files;
}

void UsdBridgeBenchmark::WriteReports()
{
    std::string jsonPath, csvPath;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_ || written_) return;
        written_ = true;
        jsonPath = outDir_ + "/benchmark_rank_" + std::to_string(rank_) + ".json";
        csvPath = outDir_ + "/benchmark_rank_" + std::to_string(rank_) + ".csv";
    }

    // Compute aggregates.
    size_t totalRaw = 0, totalWire = 0, totalDisk = 0;
    double totalSerializeMs = 0.0, totalStoreMs = 0.0;
    std::vector<double> serializeMs, storeMs, rawBytes, wireBytes;
    bool anyDisk = false, anyMemory = false;
    for (const auto& r : records_) {
        totalRaw += r.rawBytes;
        if (r.diskMode) { totalDisk += r.rawBytes; anyDisk = true; }
        else {
            totalWire += r.wireBytes;
            anyMemory = true;
            serializeMs.push_back(r.serializeMs);
            rawBytes.push_back(static_cast<double>(r.rawBytes));
            wireBytes.push_back(static_cast<double>(r.wireBytes));
        }
        totalSerializeMs += r.serializeMs;
        totalStoreMs += r.storeMs;
        storeMs.push_back(r.storeMs);
    }
    double durationS = records_.empty() ? 0.0 : (records_.back().t - startEpoch_);

    std::ostringstream j;
    j << "{\n";
    j << "  \"rank\": " << rank_ << ",\n";
    j << "  \"mode\": \"" << (anyMemory && anyDisk ? "mixed" : (anyMemory ? "memory" : "disk")) << "\",\n";
    j << "  \"commit_count\": " << records_.size() << ",\n";
    j << "  \"duration_s\": " << durationS << ",\n";
    j << "  \"serving\": {\n";
    j << "    \"bytes\": " << servedBytes_ << ",\n";
    j << "    \"chunks\": " << servedChunks_ << ",\n";
    j << "    \"files\": " << servedFiles_ << "\n";
    j << "  },\n";
    j << "  \"totals\": {\n";
    j << "    \"raw_bytes\": " << totalRaw << ",\n";
    j << "    \"wire_bytes\": " << totalWire << ",\n";
    j << "    \"disk_bytes\": " << totalDisk << ",\n";
    j << "    \"serialize_ms_total\": " << totalSerializeMs << ",\n";
    j << "    \"store_ms_total\": " << totalStoreMs << "\n";
    j << "  },\n";
    j << "  \"per_commit\": {\n";
    j << "    \"serialize_ms\": " << FormatTriple(StatsOf(serializeMs)) << ",\n";
    j << "    \"store_ms\": " << FormatTriple(StatsOf(storeMs)) << ",\n";
    j << "    \"raw_bytes\": " << FormatTriple(StatsOf(rawBytes)) << ",\n";
    j << "    \"wire_bytes\": " << FormatTriple(StatsOf(wireBytes)) << "\n";
    j << "  },\n";
    j << "  \"records\": [\n";
    for (size_t i = 0; i < records_.size(); ++i) {
        const auto& r = records_[i];
        j << "    {\"seq\":" << r.seq
          << ",\"t\":" << r.t
          << ",\"stage\":\"" << JsonEscape(r.stage) << "\""
          << ",\"disk_mode\":" << (r.diskMode ? "true" : "false")
          << ",\"raw_bytes\":" << r.rawBytes
          << ",\"wire_bytes\":" << r.wireBytes
          << ",\"serialize_ms\":" << r.serializeMs
          << ",\"store_ms\":" << r.storeMs
          << "}";
        if (i + 1 < records_.size()) j << ",";
        j << "\n";
    }
    j << "  ]\n";
    j << "}\n";

    {
        std::ofstream out(jsonPath);
        if (!out) {
            std::cerr << "[Benchmark] Rank " << rank_ << ": FAILED to write " << jsonPath << std::endl;
            return;
        }
        out << j.str();
    }

    {
        std::ofstream out(csvPath);
        if (!out) {
            std::cerr << "[Benchmark] Rank " << rank_ << ": FAILED to write " << csvPath << std::endl;
            return;
        }
        out << "seq,stage,t,disk_mode,raw_bytes,wire_bytes,serialize_ms,store_ms\n";
        for (const auto& r : records_) {
            out << r.seq << ","
                << r.stage << ","
                << r.t << ","
                << (r.diskMode ? 1 : 0) << ","
                << r.rawBytes << ","
                << r.wireBytes << ","
                << r.serializeMs << ","
                << r.storeMs << "\n";
        }
    }

    std::cout << "[Benchmark] Rank " << rank_ << ": wrote " << jsonPath << " and " << csvPath
              << " (" << records_.size() << " commits, " << (totalRaw / 1048576.0) << " MB raw)"
              << std::endl;
}

std::string UsdBridgeBenchmark::SummaryJson() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    size_t totalRaw = 0, totalWire = 0, totalDisk = 0;
    double totalSerializeMs = 0.0, totalStoreMs = 0.0;
    std::vector<double> serializeMs, storeMs, rawBytes, wireBytes;
    for (const auto& r : records_) {
        totalRaw += r.rawBytes;
        if (r.diskMode) { totalDisk += r.rawBytes; }
        else {
            totalWire += r.wireBytes;
            serializeMs.push_back(r.serializeMs);
            rawBytes.push_back(static_cast<double>(r.rawBytes));
            wireBytes.push_back(static_cast<double>(r.wireBytes));
        }
        totalSerializeMs += r.serializeMs;
        totalStoreMs += r.storeMs;
        storeMs.push_back(r.storeMs);
    }
    double durationS = records_.empty() ? 0.0 : (records_.back().t - startEpoch_);

    std::ostringstream j;
    j << "{\n";
    j << "  \"rank\": " << rank_ << ",\n";
    j << "  \"enabled\": " << (enabled_ ? "true" : "false") << ",\n";
    j << "  \"commit_count\": " << records_.size() << ",\n";
    j << "  \"duration_s\": " << durationS << ",\n";
    j << "  \"serving\": {\n";
    j << "    \"bytes\": " << servedBytes_ << ",\n";
    j << "    \"chunks\": " << servedChunks_ << ",\n";
    j << "    \"files\": " << servedFiles_ << "\n";
    j << "  },\n";
    j << "  \"totals\": {\n";
    j << "    \"raw_bytes\": " << totalRaw << ",\n";
    j << "    \"wire_bytes\": " << totalWire << ",\n";
    j << "    \"disk_bytes\": " << totalDisk << ",\n";
    j << "    \"serialize_ms_total\": " << totalSerializeMs << ",\n";
    j << "    \"store_ms_total\": " << totalStoreMs << "\n";
    j << "  },\n";
    j << "  \"per_commit\": {\n";
    j << "    \"serialize_ms\": " << FormatTriple(StatsOf(serializeMs)) << ",\n";
    j << "    \"store_ms\": " << FormatTriple(StatsOf(storeMs)) << ",\n";
    j << "    \"raw_bytes\": " << FormatTriple(StatsOf(rawBytes)) << ",\n";
    j << "    \"wire_bytes\": " << FormatTriple(StatsOf(wireBytes)) << "\n";
    j << "  }\n";
    j << "}\n";
    return j.str();
}

double TimedDiskSave(const std::function<void()>& save, const std::string& name,
                     const std::string& filePath)
{
    auto t0 = std::chrono::steady_clock::now();
    save();
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    size_t bytes = 0;
    if (!filePath.empty()) {
        struct stat st;
        if (::stat(filePath.c_str(), &st) == 0) bytes = static_cast<size_t>(st.st_size);
    }
    UsdBridgeBenchmark::Instance().RecordDiskSave(name, bytes, ms);
    return ms;
}

} // namespace usd_bridge_benchmark
