// Copyright 2024 ANARI-USD Contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef USD_BRIDGE_MEMORY_STORE_H
#define USD_BRIDGE_MEMORY_STORE_H

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <cstdint>
#include <ctime>

/**
 * @brief In-memory file storage for per-rank USD, PNG, and VDB data.
 * 
 * This class provides thread-safe storage for serialized USD stages,
 * PNG textures, and VDB volumes that would normally be written to disk.
 * Each MPI rank maintains its own MemoryFileStore instance.
 * 
 * The stored data can be retrieved on-demand via ZMQ requests from
 * the laptop client.
 */
class UsdBridgeMemoryStore {
public:
    /**
     * @brief A single file entry in the memory store
     */
    struct FileEntry {
        std::string filename;          // Relative path (e.g., "clips/geom_r0_0.0.usda")
        std::vector<uint8_t> data;     // Serialized content
        std::string mime_type;         // "text/plain", "image/png", "application/vdb"
        double timestamp;              // Creation/update time
        uint64_t hash128[2];           // XXH3-128 hash of data

        size_t size() const { return data.size(); }
    };
    
private:
    std::map<std::string, FileEntry> files_;
    mutable std::mutex mutex_;
    int rank_;
    size_t total_memory_usage_;
    
public:
    /**
     * @brief Construct a memory store for a specific MPI rank
     * @param rank The MPI rank this store belongs to
     */
    explicit UsdBridgeMemoryStore(int rank);
    
    /**
     * @brief Destructor - cleans up all stored data
     */
    ~UsdBridgeMemoryStore();
    
    /**
     * @brief Store a file in memory
     * @param filename Relative path from session directory
     * @param data Pointer to data buffer
     * @param size Size of data in bytes
     * @param mime_type MIME type of content
     */
    void StoreFile(const std::string& filename,
                    const void* data,
                    size_t size,
                    const std::string& mime_type);

    void StoreFile(const std::string& filename,
                    const std::string& content,
                    const std::string& mime_type = "text/plain");
    
    /**
     * @brief Retrieve a file from memory
     * @param filename Relative path to file
     * @return Pointer to FileEntry, or nullptr if not found
     */
    const FileEntry* GetFile(const std::string& filename) const;
    
    /**
     * @brief List all files in the store
     * @return Vector of filenames
     */
    std::vector<std::string> ListFiles() const;
    
    /**
     * @brief Update existing file or create if doesn't exist
     * @param filename Relative path to file
     * @param data Pointer to new data
     * @param size Size of new data
     */
    void UpdateFile(const std::string& filename, 
                    const void* data, 
                    size_t size);
    
    /**
     * @brief Remove a file from memory
     * @param filename Relative path to file
     * @return true if file existed and was removed
     */
    bool RemoveFile(const std::string& filename);
    
    /**
     * @brief Clear all files (used for frame overwrite mode)
     */
    void Clear();
    
    /**
     * @brief Get total memory usage of all stored files
     * @return Total bytes used
     */
    size_t GetTotalMemoryUsage() const;
    
    /**
     * @brief Get the rank this store belongs to
     * @return MPI rank
     */
    int GetRank() const { return rank_; }
    
    /**
     * @brief Get number of files stored
     * @return File count
     */
    size_t GetFileCount() const;
    
    /**
     * @brief Check if a file exists
     * @param filename Relative path to file
     * @return true if file exists
     */
    bool HasFile(const std::string& filename) const;
};

// Global per-rank memory store instance
extern UsdBridgeMemoryStore* g_rankMemoryStore;

/**
 * @brief Initialize the global memory store for this rank
 * @param rank MPI rank
 */
void InitializeMemoryStore(int rank);

/**
 * @brief Cleanup the global memory store
 */
void CleanupMemoryStore();

#endif // USD_BRIDGE_MEMORY_STORE_H
