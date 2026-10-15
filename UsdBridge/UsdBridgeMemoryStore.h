// Copyright 2024 ANARI-USD Contributors

#ifndef USD_BRIDGE_MEMORY_STORE_H
#define USD_BRIDGE_MEMORY_STORE_H

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <cstdint>
#include <ctime>

class UsdBridgeMemoryStore {
public:
    struct FileEntry {
        std::string filename;          // Relative path (e.g., "clips/geom_r0_0.0.usda")
        std::vector<uint8_t> data;     // Serialized content
        std::string mime_type;         // "text/plain", "image/png", "application/vdb"
        double timestamp;              // Creation/update time
        uint64_t hash128[2];           // XXH3-128 hash of data

        size_t size() const { return data.size(); }
    };

private:
    std::map<std::string, std::shared_ptr<const FileEntry>> files_;
    mutable std::mutex mutex_;
    int rank_;
    size_t total_memory_usage_;

public:
    explicit UsdBridgeMemoryStore(int rank);

    ~UsdBridgeMemoryStore();

    void StoreFile(const std::string& filename,
                    const void* data,
                    size_t size,
                    const std::string& mime_type);

    void StoreFile(const std::string& filename,
                    const std::string& content,
                    const std::string& mime_type = "text/plain");

    std::shared_ptr<const FileEntry> GetFile(const std::string& filename) const;

    std::vector<std::string> ListFiles() const;

    void UpdateFile(const std::string& filename,
                    const void* data,
                    size_t size);

    bool RemoveFile(const std::string& filename);

    void Clear();

    size_t GetTotalMemoryUsage() const;

    int GetRank() const { return rank_; }

    size_t GetFileCount() const;

    bool HasFile(const std::string& filename) const;
};

extern UsdBridgeMemoryStore* g_rankMemoryStore;

void InitializeMemoryStore(int rank);

void CleanupMemoryStore();

#endif // USD_BRIDGE_MEMORY_STORE_H
