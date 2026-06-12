#include "UsdBridgeDiffCapture.h"
#include "UsdBridgeMemoryStore.h"
#include <iostream>
#include <chrono>

// ============================================================
//  UsdBridgeDiffCapture — standalone old-state capture
// ============================================================
// Only depends on: <string>, <vector>, <map>, <mutex>, UsdBridgeMemoryStore.h
//
// Lifecycle:
//  1. TrackStageMemory() calls CapturePreStore(filename) BEFORE StoreFile
//  2. StoreFile overwrites the current data
//  3. Notification handler reads GetOldEntry() / GetOldHash128()
//  4. Commit() clears old data for that filename
// ============================================================

UsdBridgeDiffCapture& GetDiffCapture() {
    static UsdBridgeDiffCapture instance;
    return instance;
}

void UsdBridgeDiffCapture::CapturePreStore(const std::string& filename) {
    // Read the current (about-to-be-overwritten) entry from memory store
    if (!g_rankMemoryStore)
        return;

    const auto* oldEntry = g_rankMemoryStore->GetFile(filename);
    if (!oldEntry) {
        // File doesn't exist yet in store — nothing to capture
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    DiffFileEntry entry;
    entry.filename = filename;
    entry.oldSize = oldEntry->data.size();
    entry.oldTimestamp = oldEntry->timestamp;
    entry.oldHash128[0] = oldEntry->hash128[0];
    entry.oldHash128[1] = oldEntry->hash128[1];

    // Copy old data for diff engine (JUSYNC can request it over ZMQ)
    entry.oldData.reserve(oldEntry->data.size());
    entry.oldData.assign(oldEntry->data.begin(), oldEntry->data.end());

    entries_[filename] = std::move(entry);
}

const DiffFileEntry* UsdBridgeDiffCapture::GetOldEntry(const std::string& filename) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = entries_.find(filename);
    if (it != entries_.end()) {
        return &(it->second);
    }
    return nullptr;
}

const uint64_t* UsdBridgeDiffCapture::GetOldHash128(const std::string& filename) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = entries_.find(filename);
    if (it != entries_.end()) {
        return it->second.oldHash128;
    }
    return nullptr;
}

bool UsdBridgeDiffCapture::HasOldEntry(const std::string& filename) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.find(filename) != entries_.end();
}

void UsdBridgeDiffCapture::Commit(const std::string& filename) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(filename);
        if (it != entries_.end()) {
            entries_.erase(it);
        }
    }
}

void UsdBridgeDiffCapture::CommitBatch(const std::vector<std::string>& filenames) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& fname : filenames) {
        entries_.erase(fname);
    }
}

void UsdBridgeDiffCapture::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

size_t UsdBridgeDiffCapture::GetCapturedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

size_t UsdBridgeDiffCapture::GetTotalOldDataSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = 0;
    for (const auto& kv : entries_) {
        total += kv.second.oldData.size();
    }
    return total;
}
