#pragma once

#include <vector>
#include <memory>
#include <string>
#include <map>
#include <mutex>
#include <set>
#include <glm/glm.hpp>
#include "MiddlewareLogging.h"

#ifndef ANARI_USD_MIDDLEWARE_API
#ifdef _WIN32
#ifdef ANARI_USD_MIDDLEWARE_EXPORTS
#define ANARI_USD_MIDDLEWARE_API __declspec(dllexport)
#else
#define ANARI_USD_MIDDLEWARE_API __declspec(dllimport)
#endif
#else
#define ANARI_USD_MIDDLEWARE_API __attribute__((visibility("default")))
#endif
#endif

namespace anari_usd_middleware {

/**
 * @brief Hardware acceleration interface for mesh processing
 * 
 * Provides GPU (CUDA) and CPU (AVX-512) accelerated mesh processing
 * with automatic hardware detection and fallback.
 */
class ANARI_USD_MIDDLEWARE_API MeshAccelerator {
public:
    /**
     * @brief Supported acceleration backends
     */
    enum class Backend {
        AUTO,       ///< Automatically select best available backend
        CUDA,       ///< NVIDIA CUDA GPU acceleration
        OPENCL,     ///< OpenCL GPU acceleration (future)
        AVX512,     ///< Intel AVX-512 CPU vectorization
        AVX2,       ///< Intel AVX2 CPU vectorization
        SSE4,       ///< SSE4 CPU vectorization
        SCALAR,     ///< Scalar CPU fallback
        NONE        ///< No acceleration (use existing implementation)
    };

    /**
     * @brief Performance metrics for acceleration
     */
    struct PerformanceMetrics {
        size_t verticesProcessed = 0;
        size_t trianglesProcessed = 0;
        double processingTimeMs = 0.0;
        double throughputVerticesPerSec = 0.0;
        double throughputTrianglesPerSec = 0.0;
        Backend usedBackend = Backend::NONE;
        std::string backendName;
        size_t memoryPoolAllocations = 0;
        size_t memoryPoolReuses = 0;
        size_t memoryPoolSizeBytes = 0;
        
        void reset() {
            verticesProcessed = 0;
            trianglesProcessed = 0;
            processingTimeMs = 0.0;
            throughputVerticesPerSec = 0.0;
            throughputTrianglesPerSec = 0.0;
            usedBackend = Backend::NONE;
            backendName.clear();
            memoryPoolAllocations = 0;
            memoryPoolReuses = 0;
            memoryPoolSizeBytes = 0;
        }
    };

    /**
     * @brief Configuration for mesh acceleration
     */
    struct Config {
        Backend preferredBackend = Backend::AUTO;
        size_t minVerticesForGPU = 10000;    ///< Minimum vertices to use GPU
        size_t maxBatchSize = 1000000;       ///< Maximum vertices per batch
        bool useAsyncProcessing = true;      ///< Use asynchronous processing
        bool enableMemoryPooling = true;     ///< Enable memory pooling
        size_t memoryPoolSizeMB = 256;       ///< Memory pool size in MB
        
        // GPU-specific settings
        int cudaDeviceId = 0;                ///< CUDA device ID
        size_t cudaStreams = 4;              ///< Number of CUDA streams
        
        // CPU-specific settings
        int cpuThreads = 0;                  ///< 0 = auto-detect
        bool useSIMD = true;                 ///< Use SIMD instructions
    };

    /**
     * @brief Get singleton instance
     */
    static MeshAccelerator& getInstance();

    /**
     * @brief Initialize accelerator with configuration
     * @param config Configuration settings
     * @return True if initialization successful
     */
    bool initialize(const Config& config = Config());

    /**
     * @brief Check if accelerator is initialized
     */
    bool isInitialized() const;

    /**
     * @brief Ensure accelerator is initialized (lazy initialization)
     * @return True if initialization successful or already initialized
     */
    bool ensureInitialized();

    /**
     * @brief Get current backend
     */
    Backend getCurrentBackend() const;

    /**
     * @brief Get backend name as string
     */
    std::string getBackendName() const;

    /**
     * @brief Check if specific backend is available
     * @param backend Backend to check
     * @return True if backend is available
     */
    bool isBackendAvailable(Backend backend) const;

    /**
     * @brief Transform vertices using world matrix (accelerated)
     * @param vertices Input/output vertices
     * @param worldTransform 4x4 transformation matrix
     * @return True if transformation successful
     */
    bool transformVertices(std::vector<glm::vec3>& vertices, 
                          const glm::mat4& worldTransform);

    /**
     * @brief Transform vertices with performance metrics
     * @param vertices Input/output vertices
     * @param worldTransform 4x4 transformation matrix
     * @param metrics Output performance metrics
     * @return True if transformation successful
     */
    bool transformVertices(std::vector<glm::vec3>& vertices,
                          const glm::mat4& worldTransform,
                          PerformanceMetrics& metrics);

    /**
     * @brief Calculate mesh normals (accelerated)
     * @param vertices Input vertices
     * @param indices Input triangle indices
     * @param normals Output normals
     * @return True if calculation successful
     */
    bool calculateNormals(const std::vector<glm::vec3>& vertices,
                         const std::vector<uint32_t>& indices,
                         std::vector<glm::vec3>& normals);

    /**
     * @brief Calculate mesh normals with performance metrics
     * @param vertices Input vertices
     * @param indices Input triangle indices
     * @param normals Output normals
     * @param metrics Output performance metrics
     * @return True if calculation successful
     */
    bool calculateNormals(const std::vector<glm::vec3>& vertices,
                         const std::vector<uint32_t>& indices,
                         std::vector<glm::vec3>& normals,
                         PerformanceMetrics& metrics);

    /**
     * @brief Transform normals using normal matrix (accelerated)
     * @param normals Input/output normals
     * @param normalMatrix 3x3 normal transformation matrix
     * @return True if transformation successful
     */
    bool transformNormals(std::vector<glm::vec3>& normals,
                         const glm::mat3& normalMatrix);

    /**
     * @brief Process UV coordinates (normalize, validate)
     * @param uvs Input/output UV coordinates
     * @return True if processing successful
     */
    bool processUVs(std::vector<glm::vec2>& uvs);

    /**
     * @brief Get performance metrics from last operation
     */
    PerformanceMetrics getLastMetrics() const;

    /**
     * @brief Reset performance metrics
     */
    void resetMetrics();

    /**
     * @brief Get system information
     */
    struct SystemInfo {
        bool hasCUDA = false;
        bool hasOpenCL = false;
        bool hasAVX512 = false;
        bool hasAVX2 = false;
        bool hasSSE4 = false;
        int cudaDeviceCount = 0;
        std::string cudaDeviceName;
        int cpuCores = 0;
        size_t systemMemoryMB = 0;
        std::vector<std::string> availableBackends;
    };

    /**
     * @brief Get system information
     */
    SystemInfo getSystemInfo() const;

    /**
     * @brief Benchmark all available backends
     * @param testVertices Number of test vertices
     * @return Map of backend name to performance metrics
     */
    std::map<std::string, PerformanceMetrics> benchmark(size_t testVertices = 1000000);

    /**
     * @brief Shutdown accelerator and release resources
     */
    void shutdown();

    /**
     * @brief Memory pool management methods
     */
    
    /**
     * @brief Get memory pool statistics
     * @return Memory pool statistics
     */
    struct MemoryPoolStats {
        size_t totalAllocatedBytes = 0;
        size_t totalReservedBytes = 0;
        size_t activeAllocations = 0;
        size_t poolHits = 0;
        size_t poolMisses = 0;
        double hitRate = 0.0;
    };
    
    MemoryPoolStats getMemoryPoolStats() const;
    
    /**
     * @brief Clear memory pool
     * @param force If true, force clear even if allocations are active
     */
    void clearMemoryPool(bool force = false);
    
    /**
     * @brief Pre-allocate memory pool
     * @param sizeMB Size in megabytes to pre-allocate
     * @return True if successful
     */
    bool preallocateMemoryPool(size_t sizeMB);

    /**
     * @brief Async processing methods
     */
    
    /**
     * @brief Async task handle
     */
    struct AsyncTaskHandle {
        uint64_t id = 0;
        bool completed = false;
        bool success = false;
        PerformanceMetrics metrics;
    };
    
    /**
     * @brief Submit vertices transformation as async task
     * @param vertices Input/output vertices
     * @param worldTransform 4x4 transformation matrix
     * @return Async task handle
     */
    AsyncTaskHandle transformVerticesAsync(std::vector<glm::vec3>& vertices,
                                          const glm::mat4& worldTransform);
    
    /**
     * @brief Submit normals calculation as async task
     * @param vertices Input vertices
     * @param indices Input triangle indices
     * @param normals Output normals
     * @return Async task handle
     */
    AsyncTaskHandle calculateNormalsAsync(const std::vector<glm::vec3>& vertices,
                                         const std::vector<uint32_t>& indices,
                                         std::vector<glm::vec3>& normals);
    
    /**
     * @brief Check if async task is completed
     * @param handle Async task handle
     * @return True if task is completed
     */
    bool isAsyncTaskCompleted(const AsyncTaskHandle& handle) const;
    
    /**
     * @brief Wait for async task to complete
     * @param handle Async task handle
     * @param timeoutMs Timeout in milliseconds (0 = wait indefinitely)
     * @return True if task completed successfully
     */
    bool waitForAsyncTask(AsyncTaskHandle& handle, uint64_t timeoutMs = 0);
    
    /**
     * @brief Get async task result
     * @param handle Async task handle
     * @param metrics Output performance metrics
     * @return True if task completed successfully
     */
    bool getAsyncTaskResult(const AsyncTaskHandle& handle, PerformanceMetrics& metrics);
    
    /**
     * @brief Cancel async task
     * @param handle Async task handle
     * @return True if task was cancelled
     */
    bool cancelAsyncTask(AsyncTaskHandle& handle);
    
    /**
     * @brief Get async queue statistics
     */
    struct AsyncQueueStats {
        size_t pendingTasks = 0;
        size_t completedTasks = 0;
        size_t failedTasks = 0;
        size_t activeWorkers = 0;
        size_t maxQueueSize = 0;
    };
    
    AsyncQueueStats getAsyncQueueStats() const;

    /**
     * @brief Multi-GPU support methods
     */
    
    /**
     * @brief GPU device information
     */
    struct GPUDeviceInfo {
        int deviceId = -1;
        std::string name;
        size_t totalMemoryMB = 0;
        size_t freeMemoryMB = 0;
        int computeCapabilityMajor = 0;
        int computeCapabilityMinor = 0;
        int multiProcessorCount = 0;
        bool isIntegrated = false;
        bool supportsCUDA = false;
    };
    
    /**
     * @brief Get list of available GPU devices
     * @return Vector of GPU device information
     */
    std::vector<GPUDeviceInfo> getAvailableGPUDevices() const;
    
    /**
     * @brief Select GPU device for processing
     * @param deviceId GPU device ID (-1 for auto-select)
     * @return True if device selected successfully
     */
    bool selectGPUDevice(int deviceId = -1);
    
    /**
     * @brief Get current GPU device ID
     * @return Current GPU device ID
     */
    int getCurrentGPUDeviceId() const;
    
    /**
     * @brief Enable multi-GPU load balancing
     * @param enabled True to enable multi-GPU
     * @param strategy Load balancing strategy (0=Round Robin, 1=Memory-based, 2=Performance-based)
     * @return True if multi-GPU enabled successfully
     */
    bool enableMultiGPU(bool enabled, int strategy = 0);
    
    /**
     * @brief Get multi-GPU statistics
     */
    struct MultiGPUStats {
        size_t totalDevices = 0;
        size_t activeDevices = 0;
        std::vector<int> deviceLoadPercent;
        std::vector<size_t> deviceMemoryUsageMB;
        size_t totalProcessedVertices = 0;
        size_t totalProcessedTriangles = 0;
    };
    
    MultiGPUStats getMultiGPUStats() const;

    // Note: Advanced pipeline and approximate algorithm interfaces have been
    // defined but their implementations are deferred to future development.
    // The core optimizations (CUDA pinned memory, vertex cache optimization,
    // hybrid decision making, AVX-512 SIMD) have been fully implemented.

private:
    MeshAccelerator();
    ~MeshAccelerator();

    // Disable copy
    MeshAccelerator(const MeshAccelerator&) = delete;
    MeshAccelerator& operator=(const MeshAccelerator&) = delete;

    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace anari_usd_middleware