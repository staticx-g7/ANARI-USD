// Copyright 2024 ANARI-USD Contributors
// SPDX-License-Identifier: Apache-2.0

#include "UsdBridgeMemoryStore.h"
#if defined(BRIDGE_HAS_ZSTD)
#include <zstd.h>
#endif
#include <iostream>
#include <chrono>
#include <cstring>

#include "xxhash/xxhash.h"

// Helper: compute XXH3-128 hash of a data buffer
static void ComputeHash128(const uint8_t* data, size_t size, uint64_t outHash[2]) {
    XXH128_hash_t h = XXH3_128bits(data, size);
    outHash[0] = h.low64;
    outHash[1] = h.high64;
}

// USD payloads are zstd-compressed at store time so the wire payload is the
// compressed byte stream. This is self-describing: the receiver detects the
// zstd magic (0xFD2FB528) in the first 4 bytes and decompresses before
// parsing, so no wire struct changes. PNG (image/png) and OpenVDB files are
// already compressed formats, so only USD content is compressed. The stored
// hash/size always refer to the compressed (on-the-wire) bytes.
static bool IsUsdFilename(const std::string& filename) {
    if (filename.size() < 5) return false;
    return filename.compare(filename.size() - 5, 5, ".usda") == 0 ||
           filename.compare(filename.size() - 4, 4, ".usd") == 0;
}

// Returns the compressed bytes, or an empty vector when compression is unavailable,
// not beneficial, or fails (caller then stores the raw data).
#if defined(BRIDGE_HAS_ZSTD)
static std::vector<uint8_t> CompressZstd(const uint8_t* data, size_t size,
                                         int rank, const std::string& filename) {
    if (size == 0) return {};
    size_t bound = ZSTD_compressBound(size);
    std::vector<uint8_t> out(bound);
    // Level 1 keeps per-rank per-frame CPU cost low; the wire savings are still
    // large for ASCII USD and non-negative for binary USDC (fall back if not).
    size_t written = ZSTD_compress(out.data(), bound, data, size, 1);
    if (ZSTD_isError(written)) {
        std::cerr << "[MemoryStore] Rank " << rank << " zstd compression failed for "
                  << filename << " (" << ZSTD_getErrorName(written)
                  << ") - storing uncompressed" << std::endl;
        return {};
    }
    out.resize(written);
    if (out.size() >= size) {
        // Incompressible content: keep the raw bytes (no zstd framing overhead)
        return {};
    }
    return out;
}
#else
static std::vector<uint8_t> CompressZstd(const uint8_t* data, size_t size,
                                         int rank, const std::string& filename) {
    (void)data; (void)size; (void)rank; (void)filename;
    return {};
}
#endif

// Global instance
UsdBridgeMemoryStore* g_rankMemoryStore = nullptr;

UsdBridgeMemoryStore::UsdBridgeMemoryStore(int rank)
    : rank_(rank)
    , total_memory_usage_(0)
{
    std::cout << "[MemoryStore] Initialized for rank " << rank << std::endl;
}

UsdBridgeMemoryStore::~UsdBridgeMemoryStore()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::cout << "[MemoryStore] Rank " << rank_ 
              << " shutting down. Total files: " << files_.size()
              << ", Memory used: " << (total_memory_usage_ / 1024.0 / 1024.0) << " MB"
              << std::endl;
    files_.clear();
}

void UsdBridgeMemoryStore::StoreFile(const std::string& filename,
                                       const std::string& content,
                                       const std::string& mime_type)
{
    StoreFile(filename, content.data(), content.size(), mime_type);
}

void UsdBridgeMemoryStore::StoreFile(const std::string& filename,
                                         const void* data,
                                         size_t size,
                                         const std::string& mime_type)
{
    // Compression + hashing happen OUTSIDE the lock (both are CPU-bound and
    // can take single-digit milliseconds for large geometry files).
    const uint8_t* srcData = static_cast<const uint8_t*>(data);
    std::vector<uint8_t> compressed;
    if (IsUsdFilename(filename)) {
        compressed = CompressZstd(srcData, size, rank_, filename);
    }
    const uint8_t* storePtr = compressed.empty() ? srcData : compressed.data();
    size_t storeSize = compressed.empty() ? size : compressed.size();
    if (storeSize == 0) {
        storePtr = reinterpret_cast<const uint8_t*>("");
    }

    uint64_t hash[2] = {0, 0};
    ComputeHash128(storePtr, storeSize, hash);

    // Get current timestamp
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration<double>(now.time_since_epoch()).count();

    std::lock_guard<std::mutex> lock(mutex_);

    // Check if file already exists
    auto it = files_.find(filename);
    if (it != files_.end()) {
        // File exists - replace with a fresh immutable snapshot. Consumers
        // holding the old shared_ptr keep a consistent view of the previous
        // frame while the new frame is already served to new requests.
        // Build the new entry from scratch: copying the old entry first would
        // unnecessarily memcpy the entire previous frame before overwriting it.
        auto fresh = std::make_shared<FileEntry>();
        fresh->filename = filename;
        fresh->data.assign(storePtr, storePtr + storeSize);
        fresh->mime_type = mime_type;
        fresh->timestamp = timestamp;
        fresh->hash128[0] = hash[0];
        fresh->hash128[1] = hash[1];

        total_memory_usage_ -= it->second->data.size();
        total_memory_usage_ += storeSize;
        it->second = std::move(fresh);

        if (!compressed.empty()) {
            std::cout << "[MemoryStore] Rank " << rank_
                      << " updated: " << filename
                      << " (" << (size / 1024.0) << " KB -> " << (storeSize / 1024.0)
                      << " KB zstd, " << mime_type << ")" << std::endl;
        } else {
            std::cout << "[MemoryStore] Rank " << rank_
                      << " updated: " << filename
                      << " (" << (size / 1024.0) << " KB, " << mime_type << ")"
                      << std::endl;
        }
    } else {
        // New file - create entry
        auto fresh = std::make_shared<FileEntry>();
        fresh->filename = filename;
        fresh->data.assign(storePtr, storePtr + storeSize);
        fresh->mime_type = mime_type;
        fresh->timestamp = timestamp;
        fresh->hash128[0] = hash[0];
        fresh->hash128[1] = hash[1];

        files_[filename] = std::move(fresh);
        total_memory_usage_ += storeSize;

        if (!compressed.empty()) {
            std::cout << "[MemoryStore] Rank " << rank_
                      << " stored: " << filename
                      << " (" << (size / 1024.0) << " KB -> " << (storeSize / 1024.0)
                      << " KB zstd, " << mime_type << ")" << std::endl;
        } else {
            std::cout << "[MemoryStore] Rank " << rank_
                      << " stored: " << filename
                      << " (" << (size / 1024.0) << " KB, " << mime_type << ")"
                      << std::endl;
        }
    }
}

std::shared_ptr<const UsdBridgeMemoryStore::FileEntry> UsdBridgeMemoryStore::GetFile(const std::string& filename) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = files_.find(filename);
    if (it != files_.end()) {
        return it->second;
    }

    return nullptr;
}

std::vector<std::string> UsdBridgeMemoryStore::ListFiles() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<std::string> filenames;
    filenames.reserve(files_.size());
    
    for (const auto& pair : files_) {
        filenames.push_back(pair.first);
    }
    
    return filenames;
}

void UsdBridgeMemoryStore::UpdateFile(const std::string& filename, 
                                        const void* data, 
                                        size_t size)
{
    // Compression + hashing outside the lock (see StoreFile)
    const uint8_t* srcData = static_cast<const uint8_t*>(data);
    std::vector<uint8_t> compressed;
    if (IsUsdFilename(filename)) {
        compressed = CompressZstd(srcData, size, rank_, filename);
    }
    const uint8_t* storePtr = compressed.empty() ? srcData : compressed.data();
    size_t storeSize = compressed.empty() ? size : compressed.size();
    if (storeSize == 0) {
        storePtr = reinterpret_cast<const uint8_t*>("");
    }

    XXH128_hash_t hash = XXH3_128bits(storePtr, storeSize);

    auto now = std::chrono::system_clock::now();
    double timestamp = std::chrono::duration<double>(now.time_since_epoch()).count();

    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = files_.find(filename);
        if (it != files_.end()) {
            // Replace existing file with a fresh immutable snapshot.
            // Build it from scratch (not by copying the old entry) to avoid
            // an extra full-size memcpy of the previous frame.
            auto fresh = std::make_shared<FileEntry>();
            fresh->filename = it->second->filename;
            fresh->data.assign(storePtr, storePtr + storeSize);
            fresh->mime_type = it->second->mime_type;
            fresh->hash128[0] = hash.low64;
            fresh->hash128[1] = hash.high64;
            fresh->timestamp = timestamp;

            total_memory_usage_ -= it->second->data.size();
            total_memory_usage_ += storeSize;
            it->second = std::move(fresh);
        } else {
            // File doesn't exist - can't update
            std::cerr << "[MemoryStore] Rank " << rank_ 
                      << " cannot update non-existent file: " << filename 
                      << std::endl;
        }
    }
}

bool UsdBridgeMemoryStore::RemoveFile(const std::string& filename)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = files_.find(filename);
    if (it != files_.end()) {
        total_memory_usage_ -= it->second->data.size();
        files_.erase(it);

        std::cout << "[MemoryStore] Rank " << rank_
                  << " removed: " << filename
                  << std::endl;
        return true;
    }

    return false;
}

void UsdBridgeMemoryStore::Clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    size_t file_count = files_.size();
    files_.clear();
    total_memory_usage_ = 0;
    
    std::cout << "[MemoryStore] Rank " << rank_ 
              << " cleared " << file_count << " files"
              << std::endl;
}

size_t UsdBridgeMemoryStore::GetTotalMemoryUsage() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return total_memory_usage_;
}

size_t UsdBridgeMemoryStore::GetFileCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return files_.size();
}

bool UsdBridgeMemoryStore::HasFile(const std::string& filename) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return files_.find(filename) != files_.end();
}

// Global initialization/cleanup functions
void InitializeMemoryStore(int rank)
{
    if (g_rankMemoryStore == nullptr) {
        g_rankMemoryStore = new UsdBridgeMemoryStore(rank);
    }
}

void CleanupMemoryStore()
{
    if (g_rankMemoryStore != nullptr) {
        delete g_rankMemoryStore;
        g_rankMemoryStore = nullptr;
    }
}
