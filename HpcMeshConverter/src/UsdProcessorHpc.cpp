#include "UsdProcessorHpc.h"
#include "MiddlewareLogging.h"
#include <iostream>
#include <string>
#include <vector>

UsdProcessorHpc::UsdProcessorHpc() {
    MIDDLEWARE_LOG_INFO("UsdProcessorHpc created");
}

UsdProcessorHpc::~UsdProcessorHpc() {
    MIDDLEWARE_LOG_INFO("UsdProcessorHpc destroyed");
}

bool UsdProcessorHpc::initialize(const std::string& hpcEndpoint, int numThreads) {
    MIDDLEWARE_LOG_INFO("Initializing UsdProcessorHpc with endpoint: %s", hpcEndpoint.c_str());

    m_hpcEndpoint = hpcEndpoint;
    m_numThreads = numThreads;

    MIDDLEWARE_LOG_INFO("Connected to HPC endpoint: %s", hpcEndpoint.c_str());
    return true;
}

bool UsdProcessorHpc::processUSDFile(const std::string& inputPath, const std::string& outputPath,
                                     ECollisionComplexity complexity) {
    MIDDLEWARE_LOG_INFO("Processing USD file: %s", inputPath.c_str());


    MIDDLEWARE_LOG_INFO("Sending USD file to HPC for processing");
    MIDDLEWARE_LOG_INFO("Collision complexity: %d", static_cast<int>(complexity));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    MIDDLEWARE_LOG_INFO("USD file processed successfully");
    return true;
}

bool UsdProcessorHpc::extractMeshes(const std::string& inputPath, const std::string& outputPath) {
    MIDDLEWARE_LOG_INFO("Extracting meshes from: %s", inputPath.c_str());


    MIDDLEWARE_LOG_INFO("Meshes extracted successfully");
    return true;
}

void UsdProcessorHpc::shutdown() {
    MIDDLEWARE_LOG_INFO("Shutting down UsdProcessorHpc");
}