#include "AnariUsdMiddleware_C.h"
#include "AnariUsdMessages.h"
#include "AnariUsdClient.h"
#include "ZmqConnector.h"
#include "MiddlewareLogging.h"
#include "UsdProcessor.h"

#include <cstring>
#include <sstream>
#include <mutex>
#include <thread>
#include <atomic>
#include <memory>
#include <vector>
#include <map>
#include <functional>

// ============================================================================
// Global Middleware State
// ============================================================================

static std::unique_ptr<anari_usd_middleware::AnariUsdClient> g_anariClient;
static std::unique_ptr<anari_usd_middleware::ZmqConnector> g_zmqConnector;
static std::unique_ptr<UsdProcessor> g_usdProcessor;

static std::atomic<bool> g_isInitialized{false};
static std::atomic<bool> g_isReceiving{false};
static std::thread g_receiverThread;

static std::mutex g_mutex;

// Callback storage
static FileReceivedCallback_C g_fileCallback = nullptr;
static MessageReceivedCallback_C g_messageCallback = nullptr;
static ParallelFileReceivedCallback_C g_parallelFileCallback = nullptr;
static ParallelDownloadCompleteCallback_C g_parallelCompleteCallback = nullptr;
static ParallelDownloadErrorCallback_C g_parallelErrorCallback = nullptr;

// Status buffer
static std::string g_statusBuffer;

// ============================================================================
// CORE MIDDLEWARE FUNCTIONS
// ============================================================================

extern "C" {

int InitializeMiddleware_C(const char* endpoint) {
    std::lock_guard<std::mutex> lock(g_mutex);
    
    MIDDLEWARE_LOG_INFO("InitializeMiddleware_C called with endpoint: %s", 
                        endpoint ? endpoint : "(null)");
    
    try {
        if (g_isInitialized.load()) {
            MIDDLEWARE_LOG_WARNING("Middleware already initialized, reinitializing");
            ShutdownMiddleware_C();
        }
        
        // Create ANARI USD Client
        g_anariClient = std::make_unique<anari_usd_middleware::AnariUsdClient>();
        
        // Create ZMQ Connector
        g_zmqConnector = std::make_unique<anari_usd_middleware::ZmqConnector>();
        
        // Create USD Processor
        g_usdProcessor = std::make_unique<UsdProcessor>();
        
        // Store endpoint if provided
        std::string ep = endpoint ? endpoint : "tcp://*:5556";
        
        // Initialize ZMQ connector
        if (g_zmqConnector) {
            if (!g_zmqConnector->initialize(ep.c_str(), 5000)) {
                MIDDLEWARE_LOG_ERROR("Failed to initialize ZMQ connector");
                return 0;
            }
        }
        
        g_isInitialized.store(true);
        
        // Update status
        {
            std::ostringstream oss;
            oss << "Initialized (endpoint: " << ep << ")";
            g_statusBuffer = oss.str();
        }
        
        MIDDLEWARE_LOG_INFO("Middleware initialized successfully");
        return 1;
        
    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception during initialization: %s", e.what());
        return 0;
    }
}

void ShutdownMiddleware_C(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    
    MIDDLEWARE_LOG_INFO("ShutdownMiddleware_C called");
    
    // Stop receiving first
    StopReceiving_C();
    
    // Disconnect from broker
    if (g_anariClient) {
        g_anariClient->disconnect(1000);
    }
    
    // Clear status
    g_statusBuffer = "Shutdown";
    
    // Destroy objects
    g_usdProcessor.reset();
    g_zmqConnector.reset();
    g_anariClient.reset();
    
    g_isInitialized.store(false);
    
    MIDDLEWARE_LOG_INFO("Middleware shutdown complete");
}

int IsConnected_C(void) {
    if (!g_isInitialized.load()) return 0;
    
    if (g_anariClient) {
        return g_anariClient->isConnected() ? 1 : 0;
    }
    
    return 0;
}

const char* GetStatusInfo_C(void) {
    if (!g_isInitialized.load()) {
        g_statusBuffer = "Not initialized";
        return g_statusBuffer.c_str();
    }
    
    std::ostringstream oss;
    oss << "Initialized: " << (g_isInitialized.load() ? "true" : "false");
    
    if (g_anariClient) {
        auto stats = g_anariClient->getConnectionStats();
        oss << ", Connected: " << (g_anariClient->isConnected() ? "true" : "false");
        oss << ", Requests: " << stats.totalRequestsSent;
        oss << ", Responses: " << stats.totalResponsesReceived;
        oss << ", Bytes: " << stats.totalBytesReceived;
        oss << ", Errors: " << stats.failedRequests;
    }
    
    g_statusBuffer = oss.str();
    return g_statusBuffer.c_str();
}

int StartReceiving_C(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    
    MIDDLEWARE_LOG_INFO("StartReceiving_C called");
    
    if (!g_isInitialized.load()) {
        MIDDLEWARE_LOG_ERROR("Cannot start receiving - not initialized");
        return 0;
    }
    
    if (g_isReceiving.load()) {
        MIDDLEWARE_LOG_WARNING("Already receiving");
        return 1;
    }
    
    g_isReceiving.store(true);
    
    // Start background receiver thread
    g_receiverThread = std::thread([]() {
        MIDDLEWARE_LOG_INFO("Receiver thread started");
        
        while (g_isReceiving.load()) {
            // Check for ZMQ messages
            if (g_zmqConnector) {
                if (g_zmqConnector->receiveAnyMessage(100)) {
                    std::string msg = g_zmqConnector->getLastReceivedMessage();
                    MIDDLEWARE_LOG_INFO("Received message: %zd bytes", msg.size());
                    
                    if (g_fileCallback) {
                        CFileData fileData;
                        memset(&fileData, 0, sizeof(fileData));
                        strncpy(fileData.filename, "(message)", sizeof(fileData.filename) - 1);
                        g_fileCallback(&fileData);
                    }
                    
                    if (g_messageCallback) {
                        g_messageCallback(msg.c_str());
                    }
                }
            }
        }
        
        MIDDLEWARE_LOG_INFO("Receiver thread stopped");
    });
    
    MIDDLEWARE_LOG_INFO("StartReceiving_C success");
    return 1;
}

void StopReceiving_C(void) {
    MIDDLEWARE_LOG_INFO("StopReceiving_C called");
    
    g_isReceiving.store(false);
    
    if (g_receiverThread.joinable()) {
        g_receiverThread.join();
    }
    
    MIDDLEWARE_LOG_INFO("StopReceiving_C done");
}

// ============================================================================
// CALLBACK REGISTRATION FUNCTIONS
// ============================================================================

void RegisterUpdateCallback_C(FileReceivedCallback_C callback) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_fileCallback = callback;
    MIDDLEWARE_LOG_INFO("Registered file update callback: %p", (void*)callback);
}

void RegisterMessageCallback_C(MessageReceivedCallback_C callback) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_messageCallback = callback;
    MIDDLEWARE_LOG_INFO("Registered message callback: %p", (void*)callback);
}

// ============================================================================
// BROKER CONNECTION AND FILE REQUEST FUNCTIONS
// ============================================================================

int ConnectToBroker_C(const char* broker_endpoint, int timeout_ms) {
    std::lock_guard<std::mutex> lock(g_mutex);
    
    MIDDLEWARE_LOG_INFO("ConnectToBroker_C: endpoint=%s, timeout=%dms", 
                        broker_endpoint ? broker_endpoint : "(null)", timeout_ms);
    
    if (!g_anariClient) {
        MIDDLEWARE_LOG_ERROR("Client not initialized");
        return 0;
    }
    
    const char* ep = broker_endpoint ? broker_endpoint : "tcp://localhost:5556";
    
    if (!g_anariClient->connect(ep, timeout_ms > 0 ? timeout_ms : 5000)) {
        MIDDLEWARE_LOG_ERROR("Failed to connect to broker");
        return 0;
    }
    
    MIDDLEWARE_LOG_INFO("Connected to broker");
    return 1;
}

void DisconnectFromBroker_C(void) {
    MIDDLEWARE_LOG_INFO("DisconnectFromBroker_C called");
    
    if (g_anariClient) {
        g_anariClient->disconnect(1000);
    }
}

int IsBrokerConnected_C(void) {
    if (!g_anariClient) return 0;
    return g_anariClient->isConnected() ? 1 : 0;
}

int RequestFileList_C(int32_t target_rank, char*** out_files, size_t* out_count, int timeout_ms) {
    MIDDLEWARE_LOG_INFO("RequestFileList_C: rank=%d", target_rank);
    
    if (!g_anariClient) return 0;
    
    // Allocate default empty result
    *out_files = nullptr;
    *out_count = 0;
    
    MIDDLEWARE_LOG_WARNING("RequestFileList_C not fully implemented yet");
    return 0;
}

int RequestFileListWithSizes_C(int32_t target_rank, char*** out_names, uint64_t** out_sizes, 
                               size_t* out_count, int timeout_ms) {
    MIDDLEWARE_LOG_INFO("RequestFileListWithSizes_C: rank=%d", target_rank);
    
    if (!g_anariClient) return 0;
    
    *out_names = nullptr;
    *out_sizes = nullptr;
    *out_count = 0;
    
    MIDDLEWARE_LOG_WARNING("RequestFileListWithSizes_C not fully implemented yet");
    return 0;
}

int RequestFileListWithSizesAndRanks_C(int32_t target_rank, char*** out_names, uint64_t** out_sizes,
                                        int32_t** out_ranks, size_t* out_count, int timeout_ms) {
    MIDDLEWARE_LOG_INFO("RequestFileListWithSizesAndRanks_C: rank=%d", target_rank);
    
    if (!g_anariClient) return 0;
    
    *out_names = nullptr;
    *out_sizes = nullptr;
    *out_ranks = nullptr;
    *out_count = 0;
    
    MIDDLEWARE_LOG_WARNING("RequestFileListWithSizesAndRanks_C not fully implemented yet");
    return 0;
}

void FreeFileList_C(char** files, size_t count) {
    if (files) {
        for (size_t i = 0; i < count; i++) {
            free(files[i]);
        }
        free(files);
    }
}

void FreeFileListWithSizes_C(char** names, uint64_t* sizes, size_t count) {
    if (names) {
        for (size_t i = 0; i < count; i++) {
            free(names[i]);
        }
        free(names);
    }
    if (sizes) free(sizes);
}

void FreeFileListWithSizesAndRanks_C(char** names, uint64_t* sizes, int32_t* ranks, size_t count) {
    if (names) {
        for (size_t i = 0; i < count; i++) {
            free(names[i]);
        }
        free(names);
    }
    if (sizes) free(sizes);
    if (ranks) free(ranks);
}

int RequestFile_C(const char* filename, int32_t target_rank, unsigned char** out_data, 
                  size_t* out_size, int timeout_ms) {
    MIDDLEWARE_LOG_INFO("RequestFile_C: file=%s, rank=%d", filename ? filename : "(null)", target_rank);
    
    if (!g_anariClient || !filename || !out_data) return 0;
    
    std::vector<uint8_t> data;
    if (g_anariClient->getFileSync(filename, target_rank, data, timeout_ms)) {
        *out_data = (unsigned char*)malloc(data.size());
        if (*out_data) {
            memcpy(*out_data, data.data(), data.size());
            if (out_size) *out_size = data.size();
            MIDDLEWARE_LOG_INFO("RequestFile_C success: %zu bytes", data.size());
            return 1;
        }
    }
    
    *out_data = nullptr;
    if (out_size) *out_size = 0;
    return 0;
}

int RequestFrame_C(int32_t frame_number, int32_t target_rank, CFileData** out_files, 
                   size_t* out_count, int timeout_ms) {
    MIDDLEWARE_LOG_INFO("RequestFrame_C: frame=%d, rank=%d", frame_number, target_rank);
    *out_files = nullptr;
    if (out_count) *out_count = 0;
    return 0;
}

int RequestWorkerCountExcludingRank0_C(uint32_t* out_count, int timeout_ms) {
    MIDDLEWARE_LOG_INFO("RequestWorkerCountExcludingRank0_C");
    
    if (!g_anariClient || !out_count) return 0;
    
    uint32_t count = 0;
    if (g_anariClient->getWorkerCountSync(count, timeout_ms)) {
        *out_count = count;
        MIDDLEWARE_LOG_INFO("RequestWorkerCountExcludingRank0_C: %d workers", count);
        return 1;
    }
    
    return 0;
}

int RequestWorkerListString_C(uint32_t* out_worker_count, unsigned char** out_data, 
                              size_t* out_size, int timeout_ms) {
    MIDDLEWARE_LOG_INFO("RequestWorkerListString_C");
    
    if (!g_anariClient || !out_worker_count) return 0;
    
    *out_data = nullptr;
    if (out_size) *out_size = 0;
    
    MIDDLEWARE_LOG_WARNING("RequestWorkerListString_C not fully implemented yet");
    return 0;
}

int RequestTotalWorkerCount_C(uint32_t* out_total_count, int timeout_ms) {
    MIDDLEWARE_LOG_INFO("RequestTotalWorkerCount_C");
    
    if (!g_anariClient || !out_total_count) return 0;
    
    uint32_t count = 0;
    if (g_anariClient->getTotalWorkerCountSync(count, timeout_ms)) {
        *out_total_count = count;
        MIDDLEWARE_LOG_INFO("RequestTotalWorkerCount_C: %d total", count);
        return 1;
    }
    
    return 0;
}

// ============================================================================
// ASYNC BROKER FUNCTIONS
// ============================================================================

void RequestTotalWorkerCountAsync_C(WorkerCountCallback_C callback, 
                                    BrokerErrorCallback_C error_callback, int timeout_ms) {
    MIDDLEWARE_LOG_INFO("RequestTotalWorkerCountAsync_C");
    // Launch async worker
    std::thread([callback, error_callback, timeout_ms]() {
        uint32_t count = 0;
        if (g_anariClient && g_anariClient->getTotalWorkerCountSync(count, timeout_ms)) {
            if (callback) callback(count);
        } else {
            if (error_callback) error_callback("Failed to get worker count");
        }
    }).detach();
}

void RequestWorkerCountAsync_C(WorkerCountCallback_C callback, 
                               BrokerErrorCallback_C error_callback, int timeout_ms) {
    MIDDLEWARE_LOG_INFO("RequestWorkerCountAsync_C");
    
    std::thread([callback, error_callback, timeout_ms]() {
        uint32_t count = 0;
        if (g_anariClient && g_anariClient->getWorkerCountSync(count, timeout_ms)) {
            if (callback) callback(count);
        } else {
            if (error_callback) error_callback("Failed to get worker count");
        }
    }).detach();
}

void RequestWorkerStatusAsync_C(int32_t target_rank, WorkerStatusCallback_C callback,
                                BrokerErrorCallback_C error_callback, int timeout_ms) {
    MIDDLEWARE_LOG_INFO("RequestWorkerStatusAsync_C: rank=%d", target_rank);
    
    std::thread([target_rank, callback, error_callback, timeout_ms]() {
        // Placeholder - not fully implemented
        if (error_callback) error_callback("Worker status not yet implemented");
    }).detach();
}

void RequestFileListAsync_C(int32_t target_rank, FileListCallback_C callback,
                            BrokerErrorCallback_C error_callback, int timeout_ms) {
    MIDDLEWARE_LOG_INFO("RequestFileListAsync_C: rank=%d", target_rank);
    
    std::thread([target_rank, callback, error_callback, timeout_ms]() {
        // Placeholder - not fully implemented
        if (error_callback) error_callback("Async file list not yet implemented");
    }).detach();
}

// ============================================================================
// SCENE SNAPSHOT
// ============================================================================

void RequestSceneSnapshotAsync_C(SceneSnapshotCallback_C callback, 
                                 BrokerErrorCallback_C error_callback, int timeout_ms) {
    MIDDLEWARE_LOG_INFO("RequestSceneSnapshotAsync_C");
    
    std::thread([callback, error_callback, timeout_ms]() {
        if (!g_anariClient) {
            if (error_callback) error_callback("Client not initialized");
            return;
        }
        
        // Send scene snapshot request
        ZmqSceneSnapshotReq req;
        memset(&req, 0, sizeof(req));
        req.magic = USD_FILE_MAGIC;
        req.message_type = REQ_SCENE_SNAPSHOT;
        req.request_id = 1; // Would need proper request ID management
        
        // Send via socket
        bool sent = false;
        // TODO: Implement actual ZMQ send
        
        if (callback) {
            // Return empty but valid JSON
            const char* json = "{\"request_id\":1,\"type\":\"scene_snapshot\",\"timestamp\":0,\"num_workers\":0,\"workers\":[],\"files\":[]}";
            callback(json);
        }
    }).detach();
}

// ============================================================================
// PARALLEL FILE DOWNLOAD
// ============================================================================

void RequestFilesParallelAsync_C(const char** filenames, size_t filename_count,
                                 const int32_t* target_ranks,
                                 ParallelFileReceivedCallback_C file_received_callback,
                                 ParallelDownloadCompleteCallback_C completion_callback,
                                 ParallelDownloadErrorCallback_C error_callback,
                                 int timeout_ms) {
    MIDDLEWARE_LOG_INFO("RequestFilesParallelAsync_C: %zu files", filename_count);
    
    g_parallelFileCallback = file_received_callback;
    g_parallelCompleteCallback = completion_callback;
    g_parallelErrorCallback = error_callback;
    
    std::thread([filenames, filename_count, target_ranks, timeout_ms]() {
        std::vector<std::string> fileNames;
        std::vector<int32_t> ranks;
        
        for (size_t i = 0; i < filename_count; i++) {
            fileNames.push_back(filenames[i]);
            ranks.push_back(target_ranks[i]);
        }
        
        if (g_anariClient) {
            g_anariClient->requestFilesParallel(
                fileNames, ranks,
                [](const std::string& filename, const std::vector<uint8_t>& data) {
                    if (g_parallelFileCallback) {
                        g_parallelFileCallback(filename.c_str(), data.data(), data.size());
                    }
                },
                []() {
                    if (g_parallelCompleteCallback) {
                        g_parallelCompleteCallback();
                    }
                },
                [](const std::string& filename, const std::string& error) {
                    if (g_parallelErrorCallback) {
                        g_parallelErrorCallback(filename.c_str(), error.c_str());
                    }
                },
                timeout_ms
            );
        }
    }).detach();
}

int VerifyParallelDownloadDLL_C() {
    return 1;
}

int RequestFilesParallelDirect_C(const char** filenames, size_t filename_count,
                                 const int32_t* target_ranks,
                                 ParallelFileReceivedCallback_C file_received_callback,
                                 ParallelDownloadCompleteCallback_C completion_callback,
                                 ParallelDownloadErrorCallback_C error_callback,
                                 int timeout_ms) {
    MIDDLEWARE_LOG_INFO("RequestFilesParallelDirect_C: %zu files", filename_count);
    
    // Sync wrapper around async
    std::atomic<int> done{0};
    
    RequestFilesParallelAsync_C(filenames, filename_count, target_ranks,
                                file_received_callback,
                                [&done](void) { done.store(1); },
                                error_callback, timeout_ms);
    
    // Wait for completion
    while (!done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    return 1;
}

// ============================================================================
// MESH ACCELERATOR FUNCTIONS (Stubs)
// ============================================================================

void* CreateMeshAccelerator_C(void) { return nullptr; }
int ConfigureMeshAccelerator_C(void* accelerator, int preferred_backend, 
                               size_t min_vertices_for_gpu, int enable_async, 
                               size_t memory_pool_size_mb) { return 0; }
int TransformVerticesAccelerated_C(void* accelerator, float* vertices, 
                                   size_t vertex_count, const float* world_transform) { return 0; }
int CalculateNormalsAccelerated_C(void* accelerator, const float* vertices, 
                                  size_t vertex_count, const unsigned int* indices, 
                                  size_t index_count, float* normals) { return 0; }
int TransformNormalsAccelerated_C(void* accelerator, float* normals, 
                                  size_t normal_count, const float* normal_matrix) { return 0; }
int GetMeshAcceleratorMetrics_C(void* accelerator, size_t* out_vertices_processed, 
                                double* out_processing_time_ms, 
                                double* out_throughput_vertices_per_sec, 
                                int* out_used_backend) { return 0; }
int GetAccelerationSystemInfo_C(int* out_has_cuda, int* out_has_avx512, 
                                int* out_has_avx2, int* out_has_sse4, 
                                int* out_cpu_cores, int* out_cuda_device_count) { return 0; }
void DestroyMeshAccelerator_C(void* accelerator) {}

// ============================================================================
// PERFORMANCE OPTIMIZATION FUNCTIONS (Stubs)
// ============================================================================

int InitializeThreadPool_C(int thread_count) { return 1; }
int GetThreadPoolStats_C(int* out_thread_count, int* out_pending_tasks, int* out_active_tasks) { return 0; }
int SetMemoryPooling_C(int enable, int pool_size_mb) { return 0; }
int GetMemoryPoolStats_C(size_t* out_current_usage, size_t* out_total_allocated, size_t* out_peak_usage) { return 0; }
int ProcessUSDStreaming_C(const char* filepath, void (*callback)(CMeshData* meshes, size_t count, void* user_data),
                          void* user_data, int batch_size) { return 0; }
int SplitVerticesGPU_C(CMeshData* mesh_data, size_t* out_vertex_count) { return 0; }
int WeldVerticesGPU_C(CMeshData* mesh_data, float position_epsilon, float normal_epsilon,
                      float uv_epsilon, size_t* out_vertex_count) { return 0; }

// ============================================================================
// USD PROCESSING FUNCTIONS (Legacy)
// ============================================================================

int LoadUSDBuffer_C(const unsigned char* buffer, size_t buffer_size, const char* filename,
                    CMeshData** out_meshes, size_t* out_count) {
    return LoadUSDBufferWithCollision_C(buffer, buffer_size, filename, COLLISION_SIMPLE, out_meshes, out_count);
}

int LoadUSDFromDisk_C(const char* filepath, CMeshData** out_meshes, size_t* out_count) {
    return LoadUSDFromDiskWithCollision_C(filepath, COLLISION_SIMPLE, out_meshes, out_count);
}

// ============================================================================
// USD PROCESSING FUNCTIONS WITH COLLISION SUPPORT
// ============================================================================

int LoadUSDBufferWithCollision_C(const unsigned char* buffer, size_t buffer_size, 
                                 const char* filename, int collision_complexity,
                                 CMeshData** out_meshes, size_t* out_count) {
    MIDDLEWARE_LOG_INFO("LoadUSDBufferWithCollision_C: %s, complexity=%d", 
                        filename ? filename : "(null)", collision_complexity);
    
    if (!buffer || !buffer_size || !out_meshes) return 0;
    
    *out_meshes = nullptr;
    if (out_count) *out_count = 0;
    
    if (g_usdProcessor) {
        // Use the USD processor
        MIDDLEWARE_LOG_WARNING("LoadUSDBufferWithCollision_C - pending USD implementation");
    }
    
    return 0;
}

int LoadUSDFromDiskWithCollision_C(const char* filepath, int collision_complexity,
                                   CMeshData** out_meshes, size_t* out_count) {
    MIDDLEWARE_LOG_INFO("LoadUSDFromDiskWithCollision_C: %s", filepath ? filepath : "(null)");
    return 0;
}

// ============================================================================
// COLLISION CONFIGURATION
// ============================================================================

int SetDefaultCollisionComplexity_C(int collision_complexity) {
    if (collision_complexity >= COLLISION_NONE && collision_complexity <= COLLISION_CONVEX_DECOMP) {
        return 1;
    }
    return 0;
}

const char* GetCollisionComplexityName_C(int collision_complexity) {
    switch (collision_complexity) {
        case COLLISION_NONE: return "None";
        case COLLISION_SIMPLE: return "Simple Bounding Box";
        case COLLISION_CONVEX_HULL: return "Convex Hull";
        case COLLISION_COMPLEX: return "Complex";
        case COLLISION_SIMPLIFIED: return "Simplified Decimation";
        case COLLISION_CONVEX_DECOMP: return "V-HACD Decomposition";
        default: return "Unknown";
    }
}

int SetCollisionParameters_C(float simplification_ratio, float convex_hull_precision, int max_convex_hulls) {
    (void)simplification_ratio;
    (void)convex_hull_precision;
    (void)max_convex_hulls;
    return 1;
}

// ============================================================================
// TEXTURE PROCESSING (Stubs)
// ============================================================================

CTextureData CreateTextureFromBuffer_C(const unsigned char* buffer, size_t buffer_size) {
    CTextureData data = {0, 0, 0, nullptr, 0};
    return data;
}

int WriteGradientLineAsPNG_C(const unsigned char* buffer, size_t buffer_size, const char* output_path) {
    (void)buffer; (void)buffer_size; (void)output_path;
    return 0;
}

int GetGradientLineAsPNGBuffer_C(const unsigned char* buffer, size_t buffer_size,
                                  unsigned char** out_png_data, size_t* out_png_size) {
    (void)buffer; (void)buffer_size; (void)out_png_data; (void)out_png_size;
    return 0;
}

int GetImageRowAsPNGBuffer_C(const unsigned char* buffer, size_t buffer_size, int row_index,
                              unsigned char** out_png_data, size_t* out_png_size) {
    (void)buffer; (void)buffer_size; (void)row_index; (void)out_png_data; (void)out_png_size;
    return 0;
}

int GetPNGDimensions_C(const unsigned char* buffer, size_t buffer_size, int* out_width, 
                       int* out_height, int* out_channels) {
    (void)buffer; (void)buffer_size; (void)out_width; (void)out_height; (void)out_channels;
    return 0;
}

// ============================================================================
// MEMORY MANAGEMENT
// ============================================================================

void FreeMeshData_C(CMeshData* meshes, size_t count) {
    if (meshes) {
        for (size_t i = 0; i < count; i++) {
            if (meshes[i].points) free(meshes[i].points);
            if (meshes[i].indices) free(meshes[i].indices);
            if (meshes[i].normals) free(meshes[i].normals);
            if (meshes[i].uvs) free(meshes[i].uvs);
            if (meshes[i].vertex_colors) free(meshes[i].vertex_colors);
            if (meshes[i].collision_vertices) free(meshes[i].collision_vertices);
            if (meshes[i].collision_indices) free(meshes[i].collision_indices);
            if (meshes[i].face_vertex_counts) free(meshes[i].face_vertex_counts);
        }
        free(meshes);
    }
}

void FreeTextureData_C(CTextureData* texture) {
    if (texture && texture->data) {
        free(texture->data);
        texture->data = nullptr;
        texture->data_size = 0;
    }
}

void FreeBuffer_C(unsigned char* buffer) {
    if (buffer) free(buffer);
}

void FreeFileData_C(CFileData* file_data) {
    if (file_data && file_data->data) {
        free(file_data->data);
        file_data->data = nullptr;
    }
    free(file_data);
}

void FreeFrameFiles_C(CFileData* files, size_t count) {
    if (files) {
        for (size_t i = 0; i < count; i++) {
            FreeFileData_C(&files[i]);
        }
        free(files);
    }
}

// ============================================================================
// UTILITY AND DEBUG FUNCTIONS
// ============================================================================

const char* GetMiddlewareVersion_C(void) {
    return "1.0.0-debug";
}

int ValidateUSDFormat_C(const unsigned char* buffer, size_t buffer_size, const char* filename) {
    (void)buffer; (void)buffer_size; (void)filename;
    return 0;
}

const char* GetSupportedUSDExtensions_C(void) {
    return ".usd,.usda,.usdc,.usdz";
}

void ResetProcessingStats_C(void) {}
const char* GetProcessingStats_C(void) { return "{}"; }

} // extern "C"
