#pragma once

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <map>
#include "../ThirdParty/include/CollisionProcessor.h"
#include "../ThirdParty/include/AnariUsdClient.h"

namespace anari_usd_middleware {
    struct MeshData;
    struct CollisionData;
}

class HpcMeshConverter {
public:
    HpcMeshConverter();

    ~HpcMeshConverter();

    bool initialize(const std::string& brokerEndpoint, int rankCount = 0, bool useInfiniBand = true);

    void shutdown();

    bool processUSDFile(const std::string& usdFilePath,
                       const std::string& outputDirectory,
                       anari_usd_middleware::ECollisionComplexity collisionComplexity =
                       anari_usd_middleware::ECollisionComplexity::Complex);

    bool processUSDData(const std::vector<uint8_t>& buffer,
                       const std::string& fileName,
                       const std::string& outputDirectory,
                       anari_usd_middleware::ECollisionComplexity collisionComplexity =
                       anari_usd_middleware::ECollisionComplexity::Complex);

    struct ProcessingStats {
        uint64_t filesProcessed = 0;
        uint64_t meshesExtracted = 0;
        uint64_t errors = 0;
        uint64_t totalBytesProcessed = 0;
        std::string lastError;
    };

    ProcessingStats getStats() const;

    void setMaxConcurrentTasks(int maxTasks);

    void setTimeout(int timeoutMs);

    bool processRankUSDA(int rank,
                        const std::string& outputDirectory,
                        anari_usd_middleware::ECollisionComplexity collisionComplexity =
                        anari_usd_middleware::ECollisionComplexity::Complex);

    bool processAllRanksUSDA(const std::string& outputDirectory,
                           anari_usd_middleware::ECollisionComplexity collisionComplexity =
                           anari_usd_middleware::ECollisionComplexity::Complex);

    std::vector<std::string> getRankUSDAFiles(int rank);

    bool isInfiniBandConnected() const;

    std::vector<int> getConnectedRanks() const;

    void setInfiniBandInterface(const std::string& interfaceName);

    bool autoAssignAndProcessRanks(const std::string& outputDirectory,
                                 anari_usd_middleware::ECollisionComplexity collisionComplexity,
                                 int instanceId, int totalInstances);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;

    bool isInitialized() const;

    bool processMeshDataLocally(const std::vector<uint8_t>& buffer,
                              const std::string& fileName,
                              const std::string& outputDirectory,
                              anari_usd_middleware::ECollisionComplexity collisionComplexity);
};