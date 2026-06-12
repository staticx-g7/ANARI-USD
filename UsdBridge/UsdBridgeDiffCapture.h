/**
 * @file UsdBridgeDiffCapture.h
 * @brief Standalone diff capture layer — captures old file state before StoreFile overwrites.
 *
 * Decoupled design: only depends on UsdBridgeMemoryStore.h + standard C++ headers.
 * Drop into a newer ANARI-USD version by re-applying the 2-line hooks marked DIFF-CAPTURE-HOOK.
 */
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
    // Call BEFORE StoreFile overwrites — captures current FileEntry state
    void CapturePreStore(const std::string& filename);

    // Get old entry pointer (nullptr if file was never stored before)
    const DiffFileEntry* GetOldEntry(const std::string& filename) const;

    // Return old hash128 (returns nullptr if no old entry exists)
    const uint64_t* GetOldHash128(const std::string& filename) const;

    // Check if old data exists for this filename
    bool HasOldEntry(const std::string& filename) const;

    // Commit: clear old data after notification has been sent
    void Commit(const std::string& filename);

    // Commit: clear old data for a list of filenames
    void CommitBatch(const std::vector<std::string>& filenames);

    // Clear all captured state (e.g., on frame overwrite / restart)
    void Clear();

    // Get captured entry count (memory tracking)
    size_t GetCapturedCount() const;

    // Get total old data memory usage (approximate)
    size_t GetTotalOldDataSize() const;

    ~UsdBridgeDiffCapture() = default;

private:
    // Disable copy/move
    UsdBridgeDiffCapture() = default;
    UsdBridgeDiffCapture(const UsdBridgeDiffCapture&) = delete;
    UsdBridgeDiffCapture& operator=(const UsdBridgeDiffCapture&) = delete;

    mutable std::mutex mutex_;
    std::map<std::string, DiffFileEntry> entries_;
};

/**
 * @brief Get the singleton DiffCapture instance.
 * @return Reference to the global UsdBridgeDiffCapture.
 */
UsdBridgeDiffCapture& GetDiffCapture();

#endif // USD_BRIDGE_DIFF_CAPTURE_H
