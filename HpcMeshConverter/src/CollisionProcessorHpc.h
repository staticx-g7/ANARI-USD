#pragma once

#include <vector>
#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <chrono>
#include <glm/glm.hpp>
#include "MiddlewareLogging.h"
#include "../ThirdParty/include/AnariUsdMiddleware.h"

namespace anari_usd_middleware {
    struct CollisionData;
    enum class ECollisionComplexity : uint8_t;
}

class CollisionProcessorHpc {
public:
    CollisionProcessorHpc();

    ~CollisionProcessorHpc();

    CollisionProcessorHpc(const CollisionProcessorHpc&) = delete;
    CollisionProcessorHpc& operator=(const CollisionProcessorHpc&) = delete;
    CollisionProcessorHpc(CollisionProcessorHpc&&) = delete;
    CollisionProcessorHpc& operator=(CollisionProcessorHpc&&) = delete;

    bool generateCollision(const std::vector<float>& vertices,
                          const std::vector<uint32_t>& indices,
                          anari_usd_middleware::ECollisionComplexity complexity,
                          anari_usd_middleware::CollisionData& outCollisionData);

    bool generateCollisionForMeshes(std::vector<anari_usd_middleware::MeshData>& meshes,
                                   anari_usd_middleware::ECollisionComplexity complexity);

    bool generateCollisionForMesh(anari_usd_middleware::MeshData& mesh,
                                 anari_usd_middleware::ECollisionComplexity complexity);

    static std::string getComplexityName(anari_usd_middleware::ECollisionComplexity complexity);

    static std::string getComplexityDescription(anari_usd_middleware::ECollisionComplexity complexity);

    static std::string getComplexityRecommendation(anari_usd_middleware::ECollisionComplexity complexity);

    void setSimplificationRatio(float ratio);
    void setConvexHullPrecision(float precision);
    void setMaxConvexHulls(int maxHulls);
    void setMaxProcessingTime(float seconds);
    void setQualityVsPerformance(float balance); // 0.0 = performance, 1.0 = quality

    float getSimplificationRatio() const { return simplificationRatio_; }
    float getConvexHullPrecision() const { return convexHullPrecision_; }
    int getMaxConvexHulls() const { return maxConvexHulls_; }
    float getMaxProcessingTime() const { return maxProcessingTime_; }
    float getQualityVsPerformance() const { return qualityVsPerformance_; }

    struct ProcessingStats {
        std::atomic<uint64_t> collisionsGenerated;
        std::atomic<uint64_t> generationErrors;
        std::atomic<uint64_t> totalVerticesProcessed;
        std::atomic<uint64_t> totalTrianglesProcessed;
        std::atomic<double> averageProcessingTime;
        std::atomic<double> totalProcessingTime;
    };

    ProcessingStats getProcessingStats() const;
    void resetProcessingStats();

    void setMultiThreadingEnabled(bool enabled) { multiThreadingEnabled_ = enabled; }
    bool isMultiThreadingEnabled() const { return multiThreadingEnabled_; }

    void setProgressCallback(std::function<void(float)> callback) {
        progressCallback_ = callback;
    }

private:
    bool generateBoundingBoxCollision(const std::vector<float>& vertices,
                                     anari_usd_middleware::CollisionData& outCollisionData);

    bool generateConvexHullCollision(const std::vector<float>& vertices,
                                    const std::vector<uint32_t>& indices,
                                    anari_usd_middleware::CollisionData& outCollisionData);

    bool generateComplexCollision(const std::vector<float>& vertices,
                                 const std::vector<uint32_t>& indices,
                                 anari_usd_middleware::CollisionData& outCollisionData);

    bool generateSimplifiedCollision(const std::vector<float>& vertices,
                                    const std::vector<uint32_t>& indices,
                                    anari_usd_middleware::CollisionData& outCollisionData);

    bool generateConvexDecomposition(const std::vector<float>& vertices,
                                    const std::vector<uint32_t>& indices,
                                    anari_usd_middleware::CollisionData& outCollisionData);

    void calculateBoundingBox(const std::vector<float>& vertices,
                             glm::vec3& minBounds,
                             glm::vec3& maxBounds);

    void calculateBoundingSphere(const std::vector<float>& vertices,
                                glm::vec3& center,
                                float& radius);

    bool decimateMesh(const std::vector<float>& vertices,
                     const std::vector<uint32_t>& indices,
                     float ratio,
                     std::vector<float>& outVertices,
                     std::vector<uint32_t>& outIndices);

    bool validateCollisionMesh(const std::vector<float>& vertices,
                              const std::vector<uint32_t>& indices);

    bool validateInputMesh(const std::vector<float>& vertices,
                          const std::vector<uint32_t>& indices,
                          const std::string& context);

    void optimizeCollisionMesh(std::vector<float>& vertices,
                              std::vector<uint32_t>& indices);

    void reportProgress(float progress);
    void updateProcessingStats(bool success, size_t vertexCount, size_t triangleCount, double processingTime);

    std::atomic<float> simplificationRatio_{0.25f};    // 25% of original triangles
    std::atomic<float> convexHullPrecision_{0.001f};   // Precision for convex hull
    std::atomic<int> maxConvexHulls_{32};               // Max hulls for decomposition
    std::atomic<float> maxProcessingTime_{30.0f};      // Max processing time in seconds
    std::atomic<float> qualityVsPerformance_{0.5f};    // Quality vs performance balance
    std::atomic<bool> multiThreadingEnabled_{true};    // Enable multi-threading

    mutable std::atomic<uint64_t> collisionsGenerated_{0};
    mutable std::atomic<uint64_t> generationErrors_{0};
    mutable std::atomic<uint64_t> totalVerticesProcessed_{0};
    mutable std::atomic<uint64_t> totalTrianglesProcessed_{0};
    mutable std::atomic<double> totalProcessingTime_{0.0};

    std::function<void(float)> progressCallback_;
    mutable std::mutex callbackMutex_;

    std::atomic<bool> processingInProgress_{false};
    mutable std::chrono::steady_clock::time_point lastStatsUpdate_;
};