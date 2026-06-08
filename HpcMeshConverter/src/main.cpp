#include "HpcMeshConverter.h"
#include <iostream>
#include <string>
#include <vector>

/**
 * Main entry point for HPC Mesh Converter application
 * This application offloads mesh extraction and processing to Unreal Data on HPC
 */
int main(int argc, char* argv[]) {
    std::cout << "Starting HPC Mesh Converter application" << std::endl;
    
    // Create converter instance
    HpcMeshConverter converter;
    
    // Process command line arguments
    if (argc < 3) {
        std::cout << "HPC Mesh Converter - Process USDA data from PV server ranks via InfiniBand" << std::endl;
        std::cout << "========================================================================" << std::endl;
        std::cout << "Usage modes:" << std::endl;
        std::cout << "  1. Single rank:    HpcMeshConverter -rank <rank> -broker <endpoint> -out <dir> [-ib <interface>]" << std::endl;
        std::cout << "  2. All ranks:      HpcMeshConverter -all -broker <endpoint> -out <dir> [-ib <interface>]" << std::endl;
        std::cout << "  3. Auto-assign:    HpcMeshConverter -auto -broker <endpoint> -out <dir> -instances <N> -id <ID> [-ib <interface>]" << std::endl;
        std::cout << "  4. Legacy mode:    HpcMeshConverter <usd_file> <output_dir> [complexity]" << std::endl;
        std::cout << std::endl;
        std::cout << "Examples:" << std::endl;
        std::cout << "  Process rank 0: HpcMeshConverter -rank 0 -broker tcp://ib0:5556 -out ./output -ib ib0" << std::endl;
        std::cout << "  Process all:    HpcMeshConverter -all -broker tcp://ib0:5556 -out ./output" << std::endl;
        std::cout << "  Legacy:         HpcMeshConverter model.usd ./output 3" << std::endl;
        std::cout << std::endl;
        std::cout << "InfiniBand interfaces: ib0, ib1, mlx5_0, etc." << std::endl;
        std::cout << "Collision complexity: 0=None, 1=Simple, 2=ConvexHull, 3=Complex, 4=Simplified, 5=ConvexDecomp" << std::endl;
        return 1;
    }
    
    // Check if using new InfiniBand mode or legacy mode
    std::string firstArg = argv[1];
    bool useInfiniBandMode = (firstArg == "-rank" || firstArg == "-all" || firstArg == "-auto");
    
    if (useInfiniBandMode) {
        // New InfiniBand mode
        std::string brokerEndpoint = "tcp://localhost:5556"; // Default
        std::string outputPath = "./output";
        std::string infiniBandInterface = "ib0";
        int targetRank = -1;
        bool processAll = false;
        bool autoAssign = false;
        int instanceId = 0;
        int totalInstances = 1;
        int collisionComplexity = 3;
        
        // Parse command line arguments
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            
            if (arg == "-rank" && i + 1 < argc) {
                targetRank = std::stoi(argv[++i]);
            } else if (arg == "-all") {
                processAll = true;
            } else if (arg == "-auto") {
                autoAssign = true;
            } else if (arg == "-broker" && i + 1 < argc) {
                brokerEndpoint = argv[++i];
            } else if (arg == "-out" && i + 1 < argc) {
                outputPath = argv[++i];
            } else if (arg == "-ib" && i + 1 < argc) {
                infiniBandInterface = argv[++i];
            } else if (arg == "-complexity" && i + 1 < argc) {
                collisionComplexity = std::stoi(argv[++i]);
            } else if (arg == "-instances" && i + 1 < argc) {
                totalInstances = std::stoi(argv[++i]);
            } else if (arg == "-id" && i + 1 < argc) {
                instanceId = std::stoi(argv[++i]);
            }
        }
        
        // Validate arguments
        if (!processAll && !autoAssign && targetRank < 0) {
            std::cerr << "Error: Must specify either -rank <N>, -all, or -auto" << std::endl;
            return 1;
        }
        
        if (autoAssign && (instanceId < 0 || instanceId >= totalInstances)) {
            std::cerr << "Error: Invalid instance ID " << instanceId << " for " << totalInstances << " total instances" << std::endl;
            return 1;
        }
        
        // Initialize with InfiniBand
        std::cout << "Initializing HPC Mesh Converter with InfiniBand support..." << std::endl;
        std::cout << "  Broker endpoint: " << brokerEndpoint << std::endl;
        std::cout << "  InfiniBand interface: " << infiniBandInterface << std::endl;
        std::cout << "  Output directory: " << outputPath << std::endl;
        
        converter.setInfiniBandInterface(infiniBandInterface);
        
        // Auto-detect rank count (0 = auto-detect)
        if (!converter.initialize(brokerEndpoint, 0, true)) {
            std::cerr << "Failed to initialize HPC Mesh Converter with InfiniBand" << std::endl;
            return 1;
        }
        
        std::cout << "HPC Mesh Converter initialized successfully with InfiniBand" << std::endl;
        
        // Validate collision complexity
        anari_usd_middleware::ECollisionComplexity complexity = 
            static_cast<anari_usd_middleware::ECollisionComplexity>(collisionComplexity);
        
        bool result = false;
        if (autoAssign) {
            std::cout << "Auto-assigning ranks for instance " << instanceId << "/" << totalInstances << "..." << std::endl;
            std::cout << "Querying broker for connected workers and distributing work..." << std::endl;
            result = converter.autoAssignAndProcessRanks(outputPath, complexity, instanceId, totalInstances);
        } else if (processAll) {
            std::cout << "Processing USDA data from ALL PV server ranks sequentially..." << std::endl;
            result = converter.processAllRanksUSDA(outputPath, complexity);
        } else {
            std::cout << "Processing USDA data from rank " << targetRank << "..." << std::endl;
            result = converter.processRankUSDA(targetRank, outputPath, complexity);
        }
    } else {
        // Legacy mode for backward compatibility
        std::string inputPath = argv[1];
        std::string outputPath = argv[2];
        int collisionComplexity = 3;
        
        if (argc > 3) {
            try {
                collisionComplexity = std::stoi(argv[3]);
            } catch (...) {
                std::cout << "Invalid collision complexity, using default (3=Complex)" << std::endl;
            }
        }
        
        // Initialize with default endpoint (Ethernet fallback)
        if (!converter.initialize("tcp://localhost:5556", 8, false)) {
            std::cerr << "Failed to initialize HPC Mesh Converter" << std::endl;
            return 1;
        }
        
        std::cout << "HPC Mesh Converter initialized successfully (legacy mode)" << std::endl;
        
        // Validate collision complexity
        anari_usd_middleware::ECollisionComplexity complexity = 
            static_cast<anari_usd_middleware::ECollisionComplexity>(collisionComplexity);
        
        // Process the USD file
        std::cout << "Processing USD file: " << inputPath << std::endl;
        bool result = converter.processUSDFile(inputPath, outputPath, complexity);
    
    if (result) {
        std::cout << "USD file processed successfully" << std::endl;
        auto stats = converter.getStats();
        std::cout << "Processing completed:" << std::endl;
        std::cout << "  Files processed: " << stats.filesProcessed << std::endl;
        std::cout << "  Meshes extracted: " << stats.meshesExtracted << std::endl;
        std::cout << "  Errors: " << stats.errors << std::endl;
        std::cout << "  Total bytes processed: " << stats.totalBytesProcessed << std::endl;
    } else {
        std::cerr << "Failed to process USD file" << std::endl;
        return 1;
    }
    
    // Shutdown
    converter.shutdown();
    std::cout << "HPC Mesh Converter shutdown complete" << std::endl;
    
    return 0;
}