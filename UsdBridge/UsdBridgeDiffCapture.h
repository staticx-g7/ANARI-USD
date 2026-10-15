#ifndef USD_BRIDGE_DIFF_CAPTURE_H
#define USD_BRIDGE_DIFF_CAPTURE_H

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <cstdint>
#include <cstring>

struct DiffFileEntry {
    std::string filename;
    std::vector<uint8_t> oldData;
    uint64_t oldHash128[2];
    size_t oldSize;
    double oldTimestamp;

    DiffFileEntry()
        : oldSize(0), oldTimestamp(0)
    {
        oldHash128[0] = 0;
        oldHash128[1] = 0;
    }
};

class UsdBridgeDiffCapture {
public:
    void CapturePreStore(const std::string& filename);

    const DiffFileEntry* GetOldEntry(const std::string& filename) const;

    const uint64_t* GetOldHash128(const std::string& filename) const;

    bool HasOldEntry(const std::string& filename) const;

    void Commit(const std::string& filename);

    void CommitBatch(const std::vector<std::string>& filenames);

    void Clear();

    std::vector<std::string> GetCapturedFilenames() const;

    size_t GetCapturedCount() const;

    size_t GetTotalOldDataSize() const;

    ~UsdBridgeDiffCapture() = default;

private:
    UsdBridgeDiffCapture() = default;

    friend UsdBridgeDiffCapture& GetDiffCapture();
    UsdBridgeDiffCapture(const UsdBridgeDiffCapture&) = delete;
    UsdBridgeDiffCapture& operator=(const UsdBridgeDiffCapture&) = delete;

    mutable std::mutex mutex_;
    std::map<std::string, DiffFileEntry> entries_;
};

UsdBridgeDiffCapture& GetDiffCapture();

#endif // USD_BRIDGE_DIFF_CAPTURE_H
