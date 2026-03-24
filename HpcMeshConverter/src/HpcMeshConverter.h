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

// Forward declarations
namespace anari_usd_middleware {
    struct MeshData;
    struct CollisionData;
    // enum class ECollisionComplexity : uint8_t; // Now included from CollisionProcessor.h
}

/**
 * HPC Mesh Converter - Offloads mesh extraction and processing to HPC cluster via InfiniBand
 * This application connects to ANARI-USD broker via InfiniBand to process USDA data from each rank
 * Runs one instance per PV server rank for parallel processing
 */
class HpcMeshConverter {
public:
    /**
     * Constructor
     */
    HpcMeshConverter();
    
    /**
     * Destructor
     */
    ~HpcMeshConverter();
    
    /**
     * Initialize the converter with HPC configuration
     * @param brokerEndpoint InfiniBand endpoint for broker (e.g., "tcp://ib0:5556")
     * @param rankCount Number of PV server ranks to process (0 = auto-detect from broker)
     * @param useInfiniBand Force InfiniBand connection (true) or fallback to Ethernet (false)
     * @return True if initialization succeeded
     */
    bool initialize(const std::string& brokerEndpoint, int rankCount = 0, bool useInfiniBand = true);
    
    /**
     * Shutdown the converter
     */
    void shutdown();
    
    /**
     * Process USD file and send to HPC for mesh extraction
     * @param usdFilePath Path to the USD file
     * @param outputDirectory Directory to save processed meshes
     * @param collisionComplexity Desired collision complexity
     * @return True if processing succeeded
     */
    bool processUSDFile(const std::string& usdFilePath, 
                       const std::string& outputDirectory,
                       anari_usd_middleware::ECollisionComplexity collisionComplexity = 
                       anari_usd_middleware::ECollisionComplexity::Complex);
    
    /**
     * Process USD buffer and send to HPC for mesh extraction
     * @param buffer Raw USD data buffer
     * @param fileName Name of the file for context
     * @param outputDirectory Directory to save processed meshes
     * @param collisionComplexity Desired collision complexity
     * @return True if processing succeeded
     */
    bool processUSDData(const std::vector<uint8_t>& buffer,
                       const std::string& fileName,
                       const std::string& outputDirectory,
                       anari_usd_middleware::ECollisionComplexity collisionComplexity = 
                       anari_usd_middleware::ECollisionComplexity::Complex);
    
    /**
     * Get processing statistics
     */
    struct ProcessingStats {
        uint64_t filesProcessed = 0;
        uint64_t meshesExtracted = 0;
        uint64_t errors = 0;
        uint64_t totalBytesProcessed = 0;
        std::string lastError;
    };
    
    ProcessingStats getStats() const;
    
    /**
     * Set maximum number of concurrent HPC tasks
     */
    void setMaxConcurrentTasks(int maxTasks);
    
    /**
     * Set timeout for HPC operations
     */
    void setTimeout(int timeoutMs);
    
    /**
     * NEW: Process USDA data from specific PV server rank via InfiniBand
     * @param rank PV server rank to process (0-based)
     * @param outputDirectory Directory to save processed meshes
     * @param collisionComplexity Desired collision complexity
     * @return True if processing succeeded
     */
    bool processRankUSDA(int rank, 
                        const std::string& outputDirectory,
                        anari_usd_middleware::ECollisionComplexity collisionComplexity = 
                        anari_usd_middleware::ECollisionComplexity::Complex);
    
    /**
     * NEW: Process USDA data from all PV server ranks sequentially via InfiniBand
     * @param outputDirectory Directory to save processed meshes
     * @param collisionComplexity Desired collision complexity
     * @return True if processing succeeded
     */
    bool processAllRanksUSDA(const std::string& outputDirectory,
                           anari_usd_middleware::ECollisionComplexity collisionComplexity = 
                           anari_usd_middleware::ECollisionComplexity::Complex);
    
    /**
     * NEW: Get list of available USDA files from specific rank
     * @param rank PV server rank to query
     * @return Vector of USDA filenames available on that rank
     */
    std::vector<std::string> getRankUSDAFiles(int rank);
    
    /**
     * NEW: Check InfiniBand connection status
     * @return True if connected via InfiniBand, false if using Ethernet fallback
     */
    bool isInfiniBandConnected() const;
    
    /**
     * NEW: Get connected worker ranks from broker
     * @return Vector of available PV server ranks
     */
    std::vector<int> getConnectedRanks() const;
    
    /**
     * NEW: Set InfiniBand interface preference
     * @param interfaceName InfiniBand interface name (e.g., "ib0", "mlx5_0")
     */
    void setInfiniBandInterface(const std::string& interfaceName);
    
    /**
     * NEW: Auto-assign and process ranks based on instance ID
     * Automatically queries broker for connected workers and distributes work
     * @param outputDirectory Output directory for processed meshes
     * @param collisionComplexity Desired collision complexity
     * @param instanceId Unique ID for this instance (0-based)
     * @param totalInstances Total number of instances running
     * @return True if processing succeeded
     */
    bool autoAssignAndProcessRanks(const std::string& outputDirectory,
                                 anari_usd_middleware::ECollisionComplexity collisionComplexity,
                                 int instanceId, int totalInstances);
    
private:
    /**
     * Internal implementation class
     */
    class Impl;
    std::unique_ptr<Impl> pImpl;
    
    /**
     * Check if converter is initialized
     */
    bool isInitialized() const;
    
    /**
     * Process mesh data locally (for testing)
     */
    bool processMeshDataLocally(const std::vector<uint8_t>& buffer,
                              const std::string& fileName,
                              const std::string& outputDirectory,
                              anari_usd_middleware::ECollisionComplexity collisionComplexity);
};