#ifndef USD_BRIDGE_DIFF_CAPTURE_STATUS_H
#define USD_BRIDGE_DIFF_CAPTURE_STATUS_H

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <cstdint>
#include <chrono>

struct RankFileUpdate {
    std::string filename;             // last updated file
    std::string category;             // "geom" | "texture" | "material" | "other"
    uint64_t    timestamp;            // Unix epoch seconds from notification
    int         rank;                 // source rank that triggered it
};

class DiffCaptureStatus {
public:
    DiffCaptureStatus();

    void RegisterRank(int rank, const std::string& hostname);

    void OnNotification(int sourceRank, const char* filename, uint64_t timestamp);

    void PrintTable(
        int    clientPort,
        const std::string& brokerIP,
        int    intervalSec);

    size_t ActiveRankCount() const;

    size_t UpdatesInLastWindow(uint64_t windowSec) const;

    void Disable();

private:
    static std::string CategorizeFile(const std::string& filename);

    static uint64_t NowSec();

    mutable std::mutex                mutex_;
    std::map<int, std::string>        rankHosts_;       // rank -> hostname
    std::map<int, RankFileUpdate>     lastUpdate_;      // rank -> latest update
    std::vector<RankFileUpdate>        history_;         // tail of recent updates
    bool                              enabled_{true};
};

DiffCaptureStatus& GetDiffCaptureStatus();

#endif // USD_BRIDGE_DIFF_CAPTURE_STATUS_H
