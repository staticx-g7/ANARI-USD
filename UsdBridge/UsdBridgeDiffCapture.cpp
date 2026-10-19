#include "UsdBridgeDiffCapture.h"
#include "UsdBridgeMemoryStore.h"
#include <algorithm>
#include <iostream>
#include <chrono>


UsdBridgeDiffCapture& GetDiffCapture() {
    static UsdBridgeDiffCapture instance;
    return instance;
}

void UsdBridgeDiffCapture::CapturePreStore(const std::string& filename) {
    if (!g_rankMemoryStore)
        return;

    auto oldEntry = g_rankMemoryStore->GetFile(filename);

    std::lock_guard<std::mutex> lock(mutex_);

    DiffFileEntry entry;
    entry.filename = filename;
    entry.hasOldEntry = (oldEntry != nullptr);
    if (oldEntry) {
        entry.oldSize = oldEntry->data.size();
        entry.oldTimestamp = oldEntry->timestamp;
        entry.oldHash128[0] = oldEntry->hash128[0];
        entry.oldHash128[1] = oldEntry->hash128[1];

        entry.oldData.reserve(oldEntry->data.size());
        entry.oldData.assign(oldEntry->data.begin(), oldEntry->data.end());
    }

    // A first-time file still needs to be in the captured set: the renderFrame
    // notification loop only walks GetCapturedFilenames() when that set is
    // non-empty. Dropping new files here meant a new RawGeo sidecar (notably a
    // rebuilt "_u<k>" point cloud) was stored in the memory store but never
    // announced, so the client kept showing the previous sample.
    entries_[filename] = std::move(entry);
}

const DiffFileEntry* UsdBridgeDiffCapture::GetOldEntry(const std::string& filename) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = entries_.find(filename);
    if (it != entries_.end() && it->second.hasOldEntry) {
        return &(it->second);
    }
    return nullptr;
}

const uint64_t* UsdBridgeDiffCapture::GetOldHash128(const std::string& filename) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = entries_.find(filename);
    if (it != entries_.end() && it->second.hasOldEntry) {
        return it->second.oldHash128;
    }
    return nullptr;
}

bool UsdBridgeDiffCapture::HasOldEntry(const std::string& filename) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(filename);
    return it != entries_.end() && it->second.hasOldEntry;
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

std::vector<std::string> UsdBridgeDiffCapture::GetCapturedFilenames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> filenames;
    filenames.reserve(entries_.size());
    for (const auto& kv : entries_) {
        filenames.push_back(kv.first);
    }

    // RawGeo writes the complementary refinement subsets ("<base>_s<k>.geo")
    // BEFORE the subset-0 base ("<base>.geo") on purpose: the client only
    // emits one full cloud when the base completes the merge. A std::map
    // iteration sorts the base first ('.' < '_'), which made the client render
    // a coarse 1/S cloud and then grow it a few milliseconds later. Announce
    // the subsets first so the base lands last and emits the full cloud once.
    auto isGeoSubset = [](const std::string& f) {
        if (f.size() < 8 || f.compare(f.size() - 4, 4, ".geo") != 0) {
            return false;
        }
        size_t p = f.size() - 4; // points at '.'
        size_t d = p;
        while (d > 0 && f[d - 1] >= '0' && f[d - 1] <= '9') {
            --d;
        }
        return d < p && d >= 2 && f[d - 1] == 's' && f[d - 2] == '_';
    };
    std::stable_sort(filenames.begin(), filenames.end(),
        [&](const std::string& a, const std::string& b) {
            const bool as = isGeoSubset(a);
            const bool bs = isGeoSubset(b);
            if (as != bs) {
                return as; // subsets before base
            }
            return a < b;
        });

    return filenames;
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
