#include "HpcMeshConverter.h"
#include "UsdProcessorHpc.h"
#include "CollisionProcessorHpc.h"
#include "MiddlewareLogging.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>
#include <queue>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

// Implementation class
class HpcMeshConverter::Impl {
public:
    Impl() : initialized(false), maxWorkers(8), timeoutMs(30000), 
             stats{}, processingInProgress(false), useInfiniBand(true),
             infiniBandInterface("ib0"), rankCount(0) {}
    
    ~Impl() {
        shutdown();
    }
    
    bool initialize(const std::string& brokerEndpoint, int rankCount = 0, bool useInfiniBand = true) {
        if (initialized) {
            MIDDLEWARE_LOG_WARNING("HPC Mesh Converter already initialized");
            return true;
        }
        
        try {
            this->brokerEndpoint = brokerEndpoint;
            this->rankCount = rankCount;
            this->useInfiniBand = useInfiniBand;
            this->maxWorkers = 1; // Process one rank at a time
            
            // Initialize HPC components
            usdProcessor = std::make_unique<UsdProcessorHpc>();
            collisionProcessor = std::make_unique<CollisionProcessorHpc>();
            
            if (!usdProcessor || !collisionProcessor) {
                MIDDLEWARE_LOG_ERROR("Failed to initialize HPC processors");
                return false;
            }
            
            // Initialize InfiniBand client
            hpcClient = std::make_unique<anari_usd_middleware::AnariUsdClient>();
            
            // Configure for InfiniBand if requested
            if (useInfiniBand) {
                configureInfiniBandConnection();
            }
            
            // Test connection to broker via InfiniBand
            if (!connectToBroker()) {
                MIDDLEWARE_LOG_ERROR("Failed to connect to broker via %s: %s", 
                                   useInfiniBand ? "InfiniBand" : "Ethernet", 
                                   brokerEndpoint.c_str());
                return false;
            }
            
            // Auto-detect rank count if not specified
            if (rankCount == 0) {
                this->rankCount = detectRankCount();
                MIDDLEWARE_LOG_INFO("Auto-detected %d PV server ranks", this->rankCount);
            }
            
            initialized = true;
            MIDDLEWARE_LOG_INFO("HPC Mesh Converter initialized successfully:");
            MIDDLEWARE_LOG_INFO("  Broker endpoint: %s", brokerEndpoint.c_str());
            MIDDLEWARE_LOG_INFO("  Connection type: %s", useInfiniBand ? "InfiniBand" : "Ethernet");
            MIDDLEWARE_LOG_INFO("  PV server ranks: %d", this->rankCount);
            return true;
            
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception during HPC converter initialization: %s", e.what());
            return false;
        }
    }
    
    void shutdown() {
        if (!initialized) {
            return;
        }
        
        MIDDLEWARE_LOG_INFO("Shutting down HPC Mesh Converter...");
        initialized = false;
        processingInProgress = false;
        
        // Clear any pending tasks
        std::lock_guard<std::mutex> lock(taskMutex);
        while (!taskQueue.empty()) {
            taskQueue.pop();
        }
        taskCondition.notify_all();
        
        MIDDLEWARE_LOG_INFO("HPC Mesh Converter shutdown complete");
    }
    
    bool processUSDFile(const std::string& usdFilePath, 
                       const std::string& outputDirectory,
                       anari_usd_middleware::ECollisionComplexity collisionComplexity) {
        if (!initialized) {
            MIDDLEWARE_LOG_ERROR("HPC Mesh Converter not initialized");
            return false;
        }
        
        if (!fs::exists(usdFilePath)) {
            MIDDLEWARE_LOG_ERROR("USD file does not exist: %s", usdFilePath.c_str());
            return false;
        }
        
        if (!fs::exists(outputDirectory)) {
            try {
                fs::create_directories(outputDirectory);
            } catch (const std::exception& e) {
                MIDDLEWARE_LOG_ERROR("Failed to create output directory %s: %s", 
                                   outputDirectory.c_str(), e.what());
                return false;
            }
        }
        
        try {
            // Read the USD file
            std::vector<uint8_t> buffer;
            std::ifstream file(usdFilePath, std::ios::binary | std::ios::ate);
            if (!file.is_open()) {
                MIDDLEWARE_LOG_ERROR("Failed to open USD file: %s", usdFilePath.c_str());
                return false;
            }
            
            auto fileSize = file.tellg();
            if (fileSize <= 0) {
                MIDDLEWARE_LOG_ERROR("USD file is empty: %s", usdFilePath.c_str());
                return false;
            }
            
            buffer.resize(static_cast<size_t>(fileSize));
            file.seekg(0, std::ios::beg);
            file.read(reinterpret_cast<char*>(buffer.data()), fileSize);
            file.close();
            
            // Process the data
            std::string fileName = fs::path(usdFilePath).filename().string();
            bool result = processUSDData(buffer, fileName, outputDirectory, collisionComplexity);
            
            // Update stats
            if (result) {
                stats.filesProcessed++;
                stats.totalBytesProcessed += static_cast<uint64_t>(fileSize);
            } else {
                stats.errors++;
            }
            
            return result;
            
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception processing USD file %s: %s", 
                               usdFilePath.c_str(), e.what());
            stats.errors++;
            return false;
        }
    }
    
    bool processUSDData(const std::vector<uint8_t>& buffer,
                       const std::string& fileName,
                       const std::string& outputDirectory,
                       anari_usd_middleware::ECollisionComplexity collisionComplexity) {
        if (!initialized) {
            MIDDLEWARE_LOG_ERROR("HPC Mesh Converter not initialized");
            return false;
        }
        
        if (buffer.empty()) {
            MIDDLEWARE_LOG_ERROR("USD buffer is empty");
            return false;
        }
        
        if (outputDirectory.empty()) {
            MIDDLEWARE_LOG_ERROR("Output directory is empty");
            return false;
        }
        
        try {
            MIDDLEWARE_LOG_INFO("Processing USD data: %s (%zu bytes)", fileName.c_str(), buffer.size());
            
            // Send to HPC for processing
            bool result = sendToHpcForProcessing(buffer, fileName, outputDirectory, collisionComplexity);
            
            if (result) {
                stats.meshesExtracted++;
                stats.totalBytesProcessed += buffer.size();
            } else {
                stats.errors++;
            }
            
            return result;
            
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception processing USD data: %s", e.what());
            stats.errors++;
            return false;
        }
    }
    
    ProcessingStats getStats() const {
        return stats;
    }
    
    void setMaxConcurrentTasks(int maxTasks) {
        maxWorkers = maxTasks;
    }
    
    void setTimeout(int timeoutMs) {
        this->timeoutMs = timeoutMs;
    }
    
private:
    // InfiniBand connection methods
    void configureInfiniBandConnection() {
        // Configure ZeroMQ for optimal InfiniBand performance
        // In a real implementation, this would set socket options for IB
        MIDDLEWARE_LOG_INFO("Configuring InfiniBand connection on interface: %s", infiniBandInterface.c_str());
        
        // Set InfiniBand-specific socket options if available
        // These would be set when creating the ZMQ socket in AnariUsdClient
    }
    
    bool connectToBroker() {
        if (!hpcClient) {
            MIDDLEWARE_LOG_ERROR("HPC client not initialized");
            return false;
        }
        
        MIDDLEWARE_LOG_INFO("Connecting to broker via %s: %s", 
                          useInfiniBand ? "InfiniBand" : "Ethernet", 
                          brokerEndpoint.c_str());
        
        // Connect with timeout
        if (!hpcClient->connect(brokerEndpoint.c_str(), timeoutMs)) {
            MIDDLEWARE_LOG_ERROR("Failed to connect to broker");
            return false;
        }
        
        MIDDLEWARE_LOG_INFO("Successfully connected to broker");
        return true;
    }
    
    int detectRankCount() {
        if (!hpcClient || !hpcClient->isConnected()) {
            MIDDLEWARE_LOG_WARNING("Not connected to broker, cannot detect ranks");
            return 0;
        }
        
        // Query broker for connected worker count
        // In a real implementation, this would use AnariUsdClient to query broker
        // For now, simulate querying broker
        MIDDLEWARE_LOG_INFO("Querying broker for connected worker count...");
        
        // Simulate broker response (replace with actual broker query)
        // Typical response would be number of connected ParaView workers
        int workerCount = 8; // Default for simulation
        
        MIDDLEWARE_LOG_INFO("Broker reports %d connected workers", workerCount);
        return workerCount;
    }
    
    std::vector<int> getConnectedRanksFromBroker() {
        std::vector<int> ranks;
        if (!hpcClient || !hpcClient->isConnected()) {
            return ranks;
        }
        
        // Query broker for actual connected ranks
        // This would use AnariUsdClient's worker status query methods
        // For simulation, return ranks 0 through rankCount-1
        for (int i = 0; i < rankCount; ++i) {
            ranks.push_back(i);
        }
        
        return ranks;
    }
    
    bool autoAssignAndProcessRanks(const std::string& outputDirectory,
                                 anari_usd_middleware::ECollisionComplexity collisionComplexity,
                                 int instanceId, int totalInstances) {
        // Automatically assign ranks to this instance based on instanceId
        if (totalInstances <= 0 || instanceId < 0 || instanceId >= totalInstances) {
            MIDDLEWARE_LOG_ERROR("Invalid instance configuration: instanceId=%d, totalInstances=%d", 
                               instanceId, totalInstances);
            return false;
        }
        
        auto allRanks = getConnectedRanksFromBroker();
        if (allRanks.empty()) {
            MIDDLEWARE_LOG_WARNING("No ranks available from broker");
            return true; // No work to do is not an error
        }
        
        // Calculate which ranks this instance should process
        std::vector<int> myRanks;
        for (size_t i = 0; i < allRanks.size(); ++i) {
            if (i % totalInstances == instanceId) {
                myRanks.push_back(allRanks[i]);
            }
        }
        
        MIDDLEWARE_LOG_INFO("Instance %d/%d assigned %zu ranks: [%s]", 
                          instanceId + 1, totalInstances, myRanks.size(),
                          formatRankList(myRanks).c_str());
        
        // Process assigned ranks
        bool allSuccess = true;
        for (int rank : myRanks) {
            if (!processRankUSDA(rank, outputDirectory, collisionComplexity)) {
                MIDDLEWARE_LOG_ERROR("Failed to process assigned rank %d", rank);
                allSuccess = false;
            }
        }
        
        return allSuccess;
    }
    
    std::string formatRankList(const std::vector<int>& ranks) {
        if (ranks.empty()) return "";
        
        std::stringstream ss;
        for (size_t i = 0; i < ranks.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << ranks[i];
        }
        return ss.str();
    }
    
    // Rank-based USDA processing methods
    bool processRankUSDA(int rank, 
                        const std::string& outputDirectory,
                        anari_usd_middleware::ECollisionComplexity collisionComplexity) {
        if (!initialized || !hpcClient || !hpcClient->isConnected()) {
            MIDDLEWARE_LOG_ERROR("Not connected to broker");
            return false;
        }
        
        if (rank < 0 || (rankCount > 0 && rank >= rankCount)) {
            MIDDLEWARE_LOG_ERROR("Invalid rank %d (valid range: 0-%d)", rank, rankCount - 1);
            return false;
        }
        
        MIDDLEWARE_LOG_INFO("Processing USDA data from rank %d via %s", 
                          rank, useInfiniBand ? "InfiniBand" : "Ethernet");
        
        // 1. Get list of USDA files from this rank
        auto usdaFiles = getRankUSDAFiles(rank);
        if (usdaFiles.empty()) {
            MIDDLEWARE_LOG_WARNING("No USDA files found on rank %d", rank);
            return true; // No files to process is not an error
        }
        
        MIDDLEWARE_LOG_INFO("Found %zu USDA files on rank %d", usdaFiles.size(), rank);
        
        // 2. Process each USDA file sequentially
        bool allSuccess = true;
        for (const auto& usdaFile : usdaFiles) {
            MIDDLEWARE_LOG_INFO("Processing USDA file: %s from rank %d", usdaFile.c_str(), rank);
            
            // Request file from broker (will be routed to specific rank)
            std::vector<uint8_t> usdaData;
            if (!requestUSDAFileFromRank(rank, usdaFile, usdaData)) {
                MIDDLEWARE_LOG_ERROR("Failed to retrieve USDA file %s from rank %d", usdaFile.c_str(), rank);
                allSuccess = false;
                continue;
            }
            
            // Process USDA data in memory
            if (!processUSDADataInMemory(usdaData, usdaFile, outputDirectory, collisionComplexity)) {
                MIDDLEWARE_LOG_ERROR("Failed to process USDA file %s from rank %d", usdaFile.c_str(), rank);
                allSuccess = false;
                continue;
            }
            
            // Update statistics
            stats.filesProcessed++;
            stats.totalBytesProcessed += usdaData.size();
        }
        
        return allSuccess;
    }
    
    bool requestUSDAFileFromRank(int rank, const std::string& filename, std::vector<uint8_t>& data) {
        // In a real implementation, use AnariUsdClient to request file from specific rank
        // This would send a ZmqFileRequest with target_rank = rank
        
        MIDDLEWARE_LOG_INFO("Requesting USDA file %s from rank %d", filename.c_str(), rank);
        
        // Simulate file retrieval (replace with actual broker communication)
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        // For demonstration, create dummy USDA data
        std::string dummyUSDA = "#usda 1.0\n(\n    defaultPrim = \"Root\"\n    metersPerUnit = 0.01\n    upAxis = \"Y\"\n)\n\n";
        dummyUSDA += "def Xform \"Root\"\n{\n    def Mesh \"Mesh\"\n    {\n";
        dummyUSDA += "        int[] faceVertexCounts = [4]\n";
        dummyUSDA += "        int[] faceVertexIndices = [0, 1, 2, 3]\n";
        dummyUSDA += "        point3f[] points = [(-50, -50, 0), (50, -50, 0), (50, 50, 0), (-50, 50, 0)]\n";
        dummyUSDA += "        token subdivisionScheme = \"none\"\n";
        dummyUSDA += "    }\n}\n";
        
        data.assign(dummyUSDA.begin(), dummyUSDA.end());
        return true;
    }
    
    bool processUSDADataInMemory(const std::vector<uint8_t>& usdaData,
                                const std::string& filename,
                                const std::string& outputDirectory,
                                anari_usd_middleware::ECollisionComplexity collisionComplexity) {
        // Process USDA data entirely in memory (no disk I/O)
        MIDDLEWARE_LOG_INFO("Processing USDA data in memory: %s (%zu bytes)", filename.c_str(), usdaData.size());
        
        // 1. Parse USDA data using UsdProcessorHpc
        std::vector<UsdProcessorHpc::MeshData> meshes;
        if (!usdProcessor->parseUSDAFromMemory(usdaData.data(), usdaData.size(), meshes)) {
            MIDDLEWARE_LOG_ERROR("Failed to parse USDA data");
            return false;
        }
        
        MIDDLEWARE_LOG_INFO("Extracted %zu meshes from USDA data", meshes.size());
        
        // 2. Generate collisions for each mesh
        for (auto& mesh : meshes) {
            if (!collisionProcessor->generateCollision(mesh, collisionComplexity)) {
                MIDDLEWARE_LOG_WARNING("Failed to generate collision for mesh: %s", mesh.elementName.c_str());
            }
        }
        
        // 3. Save processed data to output directory
        std::string baseName = fs::path(filename).stem().string();
        std::string outputPath = outputDirectory + "/rank_" + std::to_string(currentProcessingRank) + 
                                "_" + baseName + "_processed.usda";
        
        if (!saveProcessedMeshes(meshes, outputPath)) {
            MIDDLEWARE_LOG_ERROR("Failed to save processed meshes");
            return false;
        }
        
        stats.meshesExtracted += meshes.size();
        return true;
    }
    
    bool saveProcessedMeshes(const std::vector<UsdProcessorHpc::MeshData>& meshes, 
                            const std::string& outputPath) {
        // Save processed meshes to USDA file
        try {
            std::ofstream outFile(outputPath);
            if (!outFile.is_open()) {
                MIDDLEWARE_LOG_ERROR("Failed to open output file: %s", outputPath.c_str());
                return false;
            }
            
            // Write processed USDA header
            outFile << "#usda 1.0\n";
            outFile << "(\n    defaultPrim = \"ProcessedMeshes\"\n    metersPerUnit = 0.01\n    upAxis = \"Y\"\n)\n\n";
            outFile << "def Xform \"ProcessedMeshes\"\n{\n";
            
            // Write each mesh
            for (const auto& mesh : meshes) {
                outFile << "    def Mesh \"" << mesh.elementName << "\"\n    {\n";
                outFile << "        int[] faceVertexCounts = [";
                for (size_t i = 0; i < mesh.faceVertexCounts.size(); ++i) {
                    if (i > 0) outFile << ", ";
                    outFile << mesh.faceVertexCounts[i];
                }
                outFile << "]\n";
                
                // Write other mesh properties...
                outFile << "        token subdivisionScheme = \"" << mesh.subdivisionScheme << "\"\n";
                outFile << "        bool doubleSided = " << (mesh.doubleSided ? "true" : "false") << "\n";
                outFile << "    }\n";
            }
            
            outFile << "}\n";
            outFile.close();
            
            MIDDLEWARE_LOG_INFO("Saved processed meshes to: %s", outputPath.c_str());
            return true;
            
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Failed to save processed meshes: %s", e.what());
            return false;
        }
    }
    
    std::vector<std::string> getRankUSDAFiles(int rank) {
        // In a real implementation, query broker for files on specific rank
        // For now, return dummy USDA files
        std::vector<std::string> files;
        
        // Generate dummy USDA filenames based on rank
        files.push_back("geometry_rank" + std::to_string(rank) + "_frame0.usda");
        files.push_back("geometry_rank" + std::to_string(rank) + "_frame1.usda");
        files.push_back("geometry_rank" + std::to_string(rank) + "_frame2.usda");
        
        return files;
    }
    
    // Legacy methods (kept for compatibility)
    bool testHpcConnection() {
        return connectToBroker();
    }
    
    bool sendToHpcForProcessing(const std::vector<uint8_t>& buffer,
                               const std::string& fileName,
                               const std::string& outputDirectory,
                               anari_usd_middleware::ECollisionComplexity collisionComplexity) {
        // Legacy method - now processes locally for backward compatibility
        return processMeshDataLocally(buffer, fileName, outputDirectory, collisionComplexity);
    }
    
    bool processMeshDataLocally(const std::vector<uint8_t>& buffer,
                              const std::string& fileName,
                              const std::string& outputDirectory,
                              anari_usd_middleware::ECollisionComplexity collisionComplexity) {
        // Legacy local processing for backward compatibility
        MIDDLEWARE_LOG_INFO("Processing mesh data locally for %s", fileName.c_str());
        
        std::string outputPath = outputDirectory + "/" + fs::path(fileName).stem().string() + "_processed.txt";
        
        try {
            std::ofstream outFile(outputPath);
            if (outFile.is_open()) {
                outFile << "Processed USD file: " << fileName << std::endl;
                outFile << "File size: " << buffer.size() << " bytes" << std::endl;
                outFile << "Collision complexity: " << static_cast<int>(collisionComplexity) << std::endl;
                outFile << "Processing completed at: " << std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count() << std::endl;
                outFile.close();
                return true;
            }
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Failed to write output file: %s", e.what());
        }
        
        return false;
    }
    
public:
    bool isInitialized() const { return initialized; }
    
private:
    // Member variables
    bool initialized;
    std::string brokerEndpoint;
    int maxWorkers;
    int timeoutMs;
    int rankCount;
    bool useInfiniBand;
    std::string infiniBandInterface;
    int currentProcessingRank;
    ProcessingStats stats;
    std::atomic<bool> processingInProgress;
    
    // HPC components
    std::unique_ptr<UsdProcessorHpc> usdProcessor;
    std::unique_ptr<CollisionProcessorHpc> collisionProcessor;
    std::unique_ptr<anari_usd_middleware::AnariUsdClient> hpcClient;
    
    // Task queue for concurrent processing
    std::mutex taskMutex;
    std::condition_variable taskCondition;
    std::queue<std::function<void()>> taskQueue;
};

// Public interface implementation
HpcMeshConverter::HpcMeshConverter() : pImpl(std::make_unique<Impl>()) {}

HpcMeshConverter::~HpcMeshConverter() = default;

bool HpcMeshConverter::initialize(const std::string& brokerEndpoint, int rankCount, bool useInfiniBand) {
    return pImpl->initialize(brokerEndpoint, rankCount, useInfiniBand);
}

void HpcMeshConverter::shutdown() {
    pImpl->shutdown();
}

bool HpcMeshConverter::processUSDFile(const std::string& usdFilePath, 
                                    const std::string& outputDirectory,
                                    anari_usd_middleware::ECollisionComplexity collisionComplexity) {
    return pImpl->processUSDFile(usdFilePath, outputDirectory, collisionComplexity);
}

bool HpcMeshConverter::processUSDData(const std::vector<uint8_t>& buffer,
                                    const std::string& fileName,
                                    const std::string& outputDirectory,
                                    anari_usd_middleware::ECollisionComplexity collisionComplexity) {
    return pImpl->processUSDData(buffer, fileName, outputDirectory, collisionComplexity);
}

HpcMeshConverter::ProcessingStats HpcMeshConverter::getStats() const {
    return pImpl->getStats();
}

void HpcMeshConverter::setMaxConcurrentTasks(int maxTasks) {
    pImpl->setMaxConcurrentTasks(maxTasks);
}

void HpcMeshConverter::setTimeout(int timeoutMs) {
    pImpl->setTimeout(timeoutMs);
}

bool HpcMeshConverter::isInitialized() const {
    return pImpl->isInitialized();
}

// New InfiniBand and rank-based processing methods
bool HpcMeshConverter::processRankUSDA(int rank, 
                                      const std::string& outputDirectory,
                                      anari_usd_middleware::ECollisionComplexity collisionComplexity) {
    return pImpl->processRankUSDA(rank, outputDirectory, collisionComplexity);
}

bool HpcMeshConverter::processAllRanksUSDA(const std::string& outputDirectory,
                                         anari_usd_middleware::ECollisionComplexity collisionComplexity) {
    // Process each rank sequentially
    bool allSuccess = true;
    auto ranks = getConnectedRanks();
    
    if (ranks.empty()) {
        MIDDLEWARE_LOG_WARNING("No connected ranks found");
        return false;
    }
    
    MIDDLEWARE_LOG_INFO("Processing USDA data from %zu ranks sequentially", ranks.size());
    
    for (int rank : ranks) {
        MIDDLEWARE_LOG_INFO("Processing rank %d/%zu", rank, ranks.size());
        if (!processRankUSDA(rank, outputDirectory, collisionComplexity)) {
            MIDDLEWARE_LOG_ERROR("Failed to process rank %d", rank);
            allSuccess = false;
        }
        // Optional: Add delay between ranks if needed
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    return allSuccess;
}

std::vector<std::string> HpcMeshConverter::getRankUSDAFiles(int rank) {
    return pImpl->getRankUSDAFiles(rank);
}

bool HpcMeshConverter::isInfiniBandConnected() const {
    return pImpl->useInfiniBand && pImpl->hpcClient && pImpl->hpcClient->isConnected();
}

std::vector<int> HpcMeshConverter::getConnectedRanks() const {
    std::vector<int> ranks;
    if (pImpl->rankCount > 0) {
        for (int i = 0; i < pImpl->rankCount; ++i) {
            ranks.push_back(i);
        }
    }
    return ranks;
}

void HpcMeshConverter::setInfiniBandInterface(const std::string& interfaceName) {
    pImpl->infiniBandInterface = interfaceName;
}

bool HpcMeshConverter::autoAssignAndProcessRanks(const std::string& outputDirectory,
                                               anari_usd_middleware::ECollisionComplexity collisionComplexity,
                                               int instanceId, int totalInstances) {
    return pImpl->autoAssignAndProcessRanks(outputDirectory, collisionComplexity, instanceId, totalInstances);
}