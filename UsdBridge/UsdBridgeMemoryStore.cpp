// Copyright 2024 ANARI-USD Contributors
// SPDX-License-Identifier: Apache-2.0

#include "UsdBridgeMemoryStore.h"
#include <iostream>
#include <chrono>
#include <cstring>

#define XXH_INLINE_ALL
#include "xxhash/xxhash.h"

// Helper: compute XXH3-128 hash of a data buffer
static void ComputeHash128(const uint8_t* data, size_t size, uint64_t outHash[2]) {
    XXH128_hash_t h = XXH3_128bits(data, size, 0);
    outHash[0] = h.low64;
    outHash[1] = h.high64;
}

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
    std::lock_guard<std::mutex> lock(mutex_);

    // Compute hash OUTSIDE lock (fast, no contention with other ops)
    uint64_t hash[2] = {0, 0};
    ComputeHash128(static_cast<const uint8_t*>(data), size, hash);

    // Get current timestamp
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration<double>(now.time_since_epoch()).count();

    // Check if file already exists
    auto it = files_.find(filename);
    if (it != files_.end()) {
        // File exists - update it
        total_memory_usage_ -= it->second.data.size();
        it->second.data.assign(static_cast<const uint8_t*>(data),
                               static_cast<const uint8_t*>(data) + size);
        it->second.mime_type = mime_type;
        it->second.timestamp = timestamp;
        it->second.hash128[0] = hash[0];
        it->second.hash128[1] = hash[1];
        total_memory_usage_ += size;

        std::cout << "[MemoryStore] Rank " << rank_
                  << " updated: " << filename
                  << " (" << (size / 1024.0) << " KB, " << mime_type << ")"
                  << std::endl;
    } else {
        // New file - create entry
        FileEntry entry;
        entry.filename = filename;
        entry.data.assign(static_cast<const uint8_t*>(data),
                           static_cast<const uint8_t*>(data) + size);
        entry.mime_type = mime_type;
        entry.timestamp = timestamp;
        entry.hash128[0] = hash[0];
        entry.hash128[1] = hash[1];

        files_[filename] = std::move(entry);
        total_memory_usage_ += size;

        std::cout << "[MemoryStore] Rank " << rank_
                  << " stored: " << filename
                  << " (" << (size / 1024.0) << " KB, " << mime_type << ")"
                  << std::endl;
    }
}

const UsdBridgeMemoryStore::FileEntry* UsdBridgeMemoryStore::GetFile(const std::string& filename) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = files_.find(filename);
    if (it != files_.end()) {
        return &(it->second);
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
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = files_.find(filename);
    if (it != files_.end()) {
        // Update existing file
        total_memory_usage_ -= it->second.data.size();
        it->second.data.assign(static_cast<const uint8_t*>(data), 
                               static_cast<const uint8_t*>(data) + size);
        total_memory_usage_ += size;
        
        auto now = std::chrono::system_clock::now();
        it->second.timestamp = std::chrono::duration<double>(now.time_since_epoch()).count();
    } else {
        // File doesn't exist - can't update
        std::cerr << "[MemoryStore] Rank " << rank_ 
                  << " cannot update non-existent file: " << filename 
                  << std::endl;
    }
}

bool UsdBridgeMemoryStore::RemoveFile(const std::string& filename)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = files_.find(filename);
    if (it != files_.end()) {
        total_memory_usage_ -= it->second.data.size();
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
