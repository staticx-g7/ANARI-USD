/**
 * @file DiffCaptureStatus.h
 * @brief Standalone rank-file update status tracker for broker terminal display.
 *
 * Tracks per-rank file update events and prints a refresh table every N seconds
 * to stdout (rank 0 terminal).  Zero dependencies on UsdBridgeZmqBroker.
 */
#ifndef USD_BRIDGE_DIFF_CAPTURE_STATUS_H
#define USD_BRIDGE_DIFF_CAPTURE_STATUS_H

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <cstdint>
#include <chrono>

/**
 * @brief Records of the latest update per filename, per rank.
 */
struct RankFileUpdate {
    std::string filename;             // last updated file
    std::string category;             // "geom" | "texture" | "material" | "other"
    uint64_t    timestamp;            // Unix epoch seconds from notification
    int         rank;                 // source rank that triggered it
};

/**
 * @brief In-memory status store; reads from notifications, writes table output.
 */
class DiffCaptureStatus {
public:
    DiffCaptureStatus();

    /** Register a known rank so it appears in the table from the start. */
    void RegisterRank(int rank, const std::string& hostname);

    /** Called on broker when a V2 notification arrives. Passes raw fields to avoid circular include. */
    void OnNotification(int sourceRank, const char* filename, uint64_t timestamp);

    /** Print the status table to stdout (clears screen with ANSI escape). */
    void PrintTable(
        int    clientPort,
        const std::string& brokerIP,
        int    intervalSec);

    /** Get count of ranks that reported at least one update. */
    size_t ActiveRankCount() const;

    /** Get total number of updates in the last `windowSec` seconds. */
    size_t UpdatesInLastWindow(uint64_t windowSec) const;

    /** Disable printing entirely (interval = 0). */
    void Disable();

private:
    /** Categorize a filename string into "geom", "texture", "material", etc. */
    static std::string CategorizeFile(const std::string& filename);

    /** Current wall-clock time in seconds since epoch. */
    static uint64_t NowSec();

    mutable std::mutex                mutex_;
    std::map<int, std::string>        rankHosts_;       // rank -> hostname
    std::map<int, RankFileUpdate>     lastUpdate_;      // rank -> latest update
    std::vector<RankFileUpdate>        history_;         // tail of recent updates
    bool                              enabled_{true};
};

/** Global singleton accessed from broker thread. */
DiffCaptureStatus& GetDiffCaptureStatus();

#endif // USD_BRIDGE_DIFF_CAPTURE_STATUS_H
