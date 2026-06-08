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

// Forward declarations
namespace anari_usd_middleware {
    struct CollisionData;
    enum class ECollisionComplexity : uint8_t;
}

/**
 * HPC Collision Processor - Optimized for HPC cluster processing
 * This class handles collision generation with enhanced performance for HPC environments
 */
class CollisionProcessorHpc {
public:
    /**
     * Constructor with enhanced safety
     */
    CollisionProcessorHpc();
    
    /**
     * Destructor with safe cleanup
     */
    ~CollisionProcessorHpc();
    
    // Disable copy/move for safety and resource management
    CollisionProcessorHpc(const CollisionProcessorHpc&) = delete;
    CollisionProcessorHpc& operator=(const CollisionProcessorHpc&) = delete;
    CollisionProcessorHpc(CollisionProcessorHpc&&) = delete;
    CollisionProcessorHpc& operator=(CollisionProcessorHpc&&) = delete;
    
    /**
     * Generate collision data from visual mesh
     * Main function for collision generation with comprehensive error handling
     *
     * @param vertices Input visual mesh vertices (flat array x,y,z,x,y,z...)
     * @param indices Input visual mesh triangle indices
     * @param complexity Desired collision complexity
     * @param outCollisionData Output collision data structure
     * @return True if generation succeeded, false otherwise
     */
    bool generateCollision(const std::vector<float>& vertices,
                          const std::vector<uint32_t>& indices,
                          anari_usd_middleware::ECollisionComplexity complexity,
                          anari_usd_middleware::CollisionData& outCollisionData);
    
    /**
     * Generate collision for multiple meshes (batch processing)
     * Optimized for processing multiple meshes from USD files
     *
     * @param meshes Input/output mesh array with collision data populated
     * @param complexity Collision complexity to apply to all meshes
     * @return True if all collisions generated successfully
     */
    bool generateCollisionForMeshes(std::vector<anari_usd_middleware::MeshData>& meshes,
                                   anari_usd_middleware::ECollisionComplexity complexity);
    
    /**
     * Generate collision for single enhanced mesh structure
     * Convenience function for single mesh processing
     *
     * @param mesh Input/output mesh with collision data populated
     * @param complexity Collision complexity to apply
     * @return True if collision generated successfully
     */
    bool generateCollisionForMesh(anari_usd_middleware::MeshData& mesh,
                                 anari_usd_middleware::ECollisionComplexity complexity);
    
    /**
     * Get string name for collision complexity
     * Essential for Unreal Engine Blueprint integration and debugging
     */
    static std::string getComplexityName(anari_usd_middleware::ECollisionComplexity complexity);
    
    /**
     * Get detailed description for collision complexity
     * Useful for tooltips and documentation in Unreal Editor
     */
    static std::string getComplexityDescription(anari_usd_middleware::ECollisionComplexity complexity);
    
    /**
     * Get recommended use case for collision complexity
     * Helps users choose appropriate complexity for their needs
     */
    static std::string getComplexityRecommendation(anari_usd_middleware::ECollisionComplexity complexity);
    
    /**
     * Set collision generation parameters for fine-tuning
     * Advanced configuration for different quality/performance trade-offs
     */
    void setSimplificationRatio(float ratio);
    void setConvexHullPrecision(float precision);
    void setMaxConvexHulls(int maxHulls);
    void setMaxProcessingTime(float seconds);
    void setQualityVsPerformance(float balance); // 0.0 = performance, 1.0 = quality
    
    /**
     * Get current configuration parameters
     */
    float getSimplificationRatio() const { return simplificationRatio_; }
    float getConvexHullPrecision() const { return convexHullPrecision_; }
    int getMaxConvexHulls() const { return maxConvexHulls_; }
    float getMaxProcessingTime() const { return maxProcessingTime_; }
    float getQualityVsPerformance() const { return qualityVsPerformance_; }
    
    /**
     * Get processing statistics
     * Useful for performance monitoring and optimization
     */
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
    
    /**
     * Enable or disable multi-threading
     * @param enabled True to enable multi-threading for batch operations
     */
    void setMultiThreadingEnabled(bool enabled) { multiThreadingEnabled_ = enabled; }
    bool isMultiThreadingEnabled() const { return multiThreadingEnabled_; }
    
    /**
     * Set progress callback for long operations
     * @param callback Function to call with progress updates (0.0 to 1.0)
     */
    void setProgressCallback(std::function<void(float)> callback) {
        progressCallback_ = callback;
    }
    
private:
    // Internal collision generation methods with enhanced error handling
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
    
    // Enhanced helper methods
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
    
    // Progress reporting
    void reportProgress(float progress);
    void updateProcessingStats(bool success, size_t vertexCount, size_t triangleCount, double processingTime);
    
    // Configuration parameters with thread safety
    std::atomic<float> simplificationRatio_{0.25f};    // 25% of original triangles
    std::atomic<float> convexHullPrecision_{0.001f};   // Precision for convex hull
    std::atomic<int> maxConvexHulls_{32};               // Max hulls for decomposition
    std::atomic<float> maxProcessingTime_{30.0f};      // Max processing time in seconds
    std::atomic<float> qualityVsPerformance_{0.5f};    // Quality vs performance balance
    std::atomic<bool> multiThreadingEnabled_{true};    // Enable multi-threading
    
    // Statistics with thread safety
    mutable std::atomic<uint64_t> collisionsGenerated_{0};
    mutable std::atomic<uint64_t> generationErrors_{0};
    mutable std::atomic<uint64_t> totalVerticesProcessed_{0};
    mutable std::atomic<uint64_t> totalTrianglesProcessed_{0};
    mutable std::atomic<double> totalProcessingTime_{0.0};
    
    // Progress callback
    std::function<void(float)> progressCallback_;
    mutable std::mutex callbackMutex_;
    
    // Internal state
    std::atomic<bool> processingInProgress_{false};
    mutable std::chrono::steady_clock::time_point lastStatsUpdate_;
};