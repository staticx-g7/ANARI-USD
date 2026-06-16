#ifndef ANARI_USD_MIDDLEWARE_C_H
#define ANARI_USD_MIDDLEWARE_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// API export/import macros for cross-platform compatibility
#ifndef ANARI_USD_MIDDLEWARE_C_API
#ifdef _WIN32
#ifdef ANARI_USD_MIDDLEWARE_EXPORTS
#define ANARI_USD_MIDDLEWARE_C_API __declspec(dllexport)
#else
#define ANARI_USD_MIDDLEWARE_C_API __declspec(dllimport)
#endif
#else
#define ANARI_USD_MIDDLEWARE_C_API __attribute__((visibility("default")))
#endif
#endif

// ============================================================================
// COLLISION COMPLEXITY ENUMERATION
// ============================================================================

/**
 * Collision complexity options for different use cases
 * These values are exposed to Unreal Engine Blueprints
 * Higher complexity = more accurate collision but slower performance
 */
typedef enum {
    COLLISION_NONE = 0,           // No collision generation
    COLLISION_SIMPLE = 1,         // Bounding box collision (fastest)
    COLLISION_CONVEX_HULL = 2,    // Convex hull around mesh (balanced)
    COLLISION_COMPLEX = 3,        // Full mesh collision (most accurate, default)
    COLLISION_SIMPLIFIED = 4,     // Decimated mesh for performance (25% triangles)
    COLLISION_CONVEX_DECOMP = 5   // V-HACD convex decomposition (best for concave shapes)
} ECollisionComplexity_C;

// ============================================================================
// C-COMPATIBLE DATA STRUCTURES
// ============================================================================

/**
 * File data structure for C interface
 * Contains received file information and binary data
 * Used by ZeroMQ callbacks when files are received
 */
typedef struct {
    char filename[256];          // Original filename (null-terminated)
    unsigned char* data;         // Binary file data (dynamically allocated)
    size_t data_size;           // Size of data in bytes
    char hash[64];              // SHA256 hash (null-terminated hex string)
    char file_type[32];         // File type identifier (e.g., "USD", "IMAGE")
} CFileData;

/**
 * Enhanced mesh data structure for C interface with collision support
 * Contains all geometric data for a single mesh primitive
 * Compatible with Unreal Engine RealtimeMeshComponent and Physics System
 *
 * Memory Layout:
 * - All arrays are flat and suitable for GPU upload
 * - Vertex data is interleaved for optimal cache performance
 * - Collision data is separate from visual mesh data
 */
typedef struct {
    char element_name[256];      // USD primitive name (null-terminated)
    char type_name[128];         // USD primitive type (null-terminated)

    // ========== VISUAL MESH DATA ==========
    // Vertex positions as flat array [x1,y1,z1, x2,y2,z2, ...]
    float* points;
    size_t points_count;         // Total number of floats (vertices * 3)

    // Triangle indices referencing vertex positions
    unsigned int* indices;
    size_t indices_count;        // Total number of indices (triangles * 3)

    // Vertex normals as flat array [nx1,ny1,nz1, nx2,ny2,nz2, ...]
    float* normals;
    size_t normals_count;        // Total number of floats (vertices * 3)

    // UV coordinates as flat array [u1,v1, u2,v2, ...]
    float* uvs;
    size_t uvs_count;           // Total number of floats (vertices * 2)

    // Vertex colors as flat RGBA array [r1,g1,b1,a1, r2,g2,b2,a2, ...]
    // Values are in range [0.0, 1.0]
    float* vertex_colors;
    size_t vertex_colors_count;  // Total number of floats (vertices * 4)

    // ========== COLLISION DATA ==========
    int collision_type;          // Maps to ECollisionComplexity_C enum

    // Collision mesh vertices (may differ from visual mesh)
    float* collision_vertices;
    size_t collision_vertices_count;  // Total number of floats (collision vertices * 3)

    // Collision mesh triangle indices
    unsigned int* collision_indices;
    size_t collision_indices_count;   // Total number of indices (collision triangles * 3)

    // Simple collision primitives (for bounding boxes, spheres)
    float bounding_box_min[3];   // Minimum bounds [x, y, z]
    float bounding_box_max[3];   // Maximum bounds [x, y, z]
    float sphere_center[3];      // Sphere center [x, y, z]
    float sphere_radius;         // Sphere radius

    // ========== USD GEOMETRY FEATURES ==========
    const char* subdivision_scheme;    // Subdivision scheme (e.g., "catmull-clark", "bilinear", "none")
    int double_sided;            // Double-sided flag (0 = false, 1 = true)

    // Face vertex counts for heterogenous polygons
    unsigned int* face_vertex_counts;
    size_t face_vertex_counts_size;

    // Multiple UV sets
    float** uv_sets;             // Array of UV set pointers (each is flat array [u,v,...])
    const char** uv_set_names;   // Array of UV set name pointers
    size_t uv_sets_count;        // Number of UV sets

} CMeshData;

/**
 * Texture data structure for C interface
 * Contains decoded image data ready for GPU upload
 * Automatically converted to RGBA format for consistency
 */
typedef struct {
    int width;                   // Image width in pixels
    int height;                  // Image height in pixels
    int channels;                // Number of channels (typically 3 or 4)
    unsigned char* data;         // Raw pixel data (dynamically allocated)
    size_t data_size;           // Size of pixel data in bytes
} CTextureData;

// ============================================================================
// CALLBACK FUNCTION TYPES
// ============================================================================

/**
 * Callback function type for file reception notifications
 * Called when a new file is received via ZeroMQ
 *
 * IMPORTANT: The file_data pointer is only valid during the callback.
 * If you need to keep the data, copy it immediately.
 *
 * @param file_data Pointer to received file data (valid only during callback)
 */
typedef void (*FileReceivedCallback_C)(const CFileData* file_data);

/**
 * Callback function type for message reception notifications
 * Called when a text message is received via ZeroMQ
 *
 * @param message Null-terminated message string (valid only during callback)
 */
typedef void (*MessageReceivedCallback_C)(const char* message);

// ============================================================================
// CORE MIDDLEWARE FUNCTIONS
// ============================================================================

/**
 * Initialize the middleware with ZeroMQ endpoint
 * Must be called before any other operations
 *
* @param endpoint ZeroMQ endpoint string (e.g., "tcp://0.0.0.0:5556") or NULL for default
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int InitializeMiddleware_C(const char* endpoint);

/**
 * Shutdown the middleware and cleanup all resources
 * Safe to call multiple times
 * Automatically stops receiver thread and disconnects ZeroMQ
 */
ANARI_USD_MIDDLEWARE_C_API void ShutdownMiddleware_C(void);

/**
 * Check if middleware is connected and ready to receive data
 * Thread-safe operation
 *
 * @return 1 if connected, 0 if not connected
 */
ANARI_USD_MIDDLEWARE_C_API int IsConnected_C(void);

/**
 * Get current status information for debugging
 * Returns connection status, statistics, and health information
 *
 * @return Pointer to status string (valid until next call)
 */
ANARI_USD_MIDDLEWARE_C_API const char* GetStatusInfo_C(void);

/**
 * Start the background receiver thread
 * Non-blocking operation that enables automatic file/message reception
 * The receiver thread handles all ZeroMQ communication
 *
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int StartReceiving_C(void);

/**
 * Stop the background receiver thread
 * Blocks until receiver thread has safely terminated
 * Safe to call multiple times
 */
ANARI_USD_MIDDLEWARE_C_API void StopReceiving_C(void);

// ============================================================================
// USD PROCESSING FUNCTIONS (Legacy - No Collision)
// ============================================================================

/**
 * Load USD data from memory buffer and extract mesh geometry (Legacy)
 * Supports .usd, .usda, .usdc, and .usdz formats
 * Extracts vertex positions, indices, normals, UVs, and vertex colors
 *
 * NOTE: This function does NOT generate collision data.
 * Use LoadUSDBufferWithCollision_C for collision support.
 *
 * @param buffer Raw USD file data
 * @param buffer_size Size of buffer in bytes
 * @param filename Original filename (used for format detection)
 * @param out_meshes Pointer to receive array of extracted meshes (caller must free)
 * @param out_count Pointer to receive number of extracted meshes
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int LoadUSDBuffer_C(const unsigned char* buffer,
                                               size_t buffer_size,
                                               const char* filename,
                                               CMeshData** out_meshes,
                                               size_t* out_count);

/**
 * Load USD data directly from disk file (Legacy)
 * Wrapper around LoadUSDBuffer_C with file I/O handling
 *
 * NOTE: This function does NOT generate collision data.
 * Use LoadUSDFromDiskWithCollision_C for collision support.
 *
 * @param filepath Path to USD file on disk
 * @param out_meshes Pointer to receive array of extracted meshes (caller must free)
 * @param out_count Pointer to receive number of extracted meshes
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int LoadUSDFromDisk_C(const char* filepath,
                                                  CMeshData** out_meshes,
                                                  size_t* out_count);

// ============================================================================
// USD PROCESSING FUNCTIONS WITH COLLISION SUPPORT
// ============================================================================

/**
 * Load USD data from memory buffer with collision generation
 * Enhanced version of LoadUSDBuffer_C with collision support
 *
 * Collision Generation Process:
 * 1. Extract visual mesh data (same as legacy function)
 * 2. Generate collision geometry based on complexity setting
 * 3. Populate collision fields in CMeshData structure
 *
 * Performance Recommendations:
 * - COLLISION_SIMPLE: Background/static objects
 * - COLLISION_COMPLEX: Interactive/detailed objects
 * - COLLISION_SIMPLIFIED: Performance-critical scenarios
 *
 * @param buffer Raw USD file data
 * @param buffer_size Size of buffer in bytes
 * @param filename Original filename (used for format detection)
 * @param collision_complexity Collision complexity level (ECollisionComplexity_C)
 * @param out_meshes Pointer to receive array of extracted meshes (caller must free)
 * @param out_count Pointer to receive number of extracted meshes
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int LoadUSDBufferWithCollision_C(const unsigned char* buffer,
                                                            size_t buffer_size,
                                                            const char* filename,
                                                            int collision_complexity,
                                                            CMeshData** out_meshes,
                                                            size_t* out_count);

/**
 * Load USD data from disk with collision generation
 * Enhanced version of LoadUSDFromDisk_C with collision support
 *
 * @param filepath Path to USD file on disk
 * @param collision_complexity Collision complexity level (ECollisionComplexity_C)
 * @param out_meshes Pointer to receive array of extracted meshes (caller must free)
 * @param out_count Pointer to receive number of extracted meshes
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int LoadUSDFromDiskWithCollision_C(const char* filepath,
                                                              int collision_complexity,
                                                              CMeshData** out_meshes,
                                                              size_t* out_count);

// ============================================================================
// COLLISION CONFIGURATION FUNCTIONS
// ============================================================================

/**
 * Set default collision complexity for future USD loading operations
 * This affects LoadUSDBufferWithCollision_C and LoadUSDFromDiskWithCollision_C
 * when collision_complexity parameter is set to -1 (use default)
 *
 * @param collision_complexity Default collision complexity level
 * @return 1 on success, 0 on failure (invalid complexity value)
 */
ANARI_USD_MIDDLEWARE_C_API int SetDefaultCollisionComplexity_C(int collision_complexity);

/**
 * Get collision complexity name for debugging and UI display
 * Useful for dropdown menus in Unreal Blueprint functions
 *
 * @param collision_complexity Collision complexity enum value
 * @return Pointer to collision name string (valid until next call)
 */
ANARI_USD_MIDDLEWARE_C_API const char* GetCollisionComplexityName_C(int collision_complexity);

/**
 * Set collision generation parameters for fine-tuning
 * Advanced configuration for collision processing
 *
 * @param simplification_ratio Ratio for simplified collision (0.1 to 0.9, default 0.25)
 * @param convex_hull_precision Precision for convex hull generation (0.001 to 0.1, default 0.001)
 * @param max_convex_hulls Maximum number of convex hulls for decomposition (1 to 64, default 32)
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int SetCollisionParameters_C(float simplification_ratio,
                                                        float convex_hull_precision,
                                                        int max_convex_hulls);

// ============================================================================
// TEXTURE PROCESSING FUNCTIONS
// ============================================================================

/**
 * Create texture data from raw image buffer
 * Supports common image formats (PNG, JPG, TGA, BMP, etc.)
 * Automatically converts to RGBA format for consistency
 *
 * @param buffer Raw image file data
 * @param buffer_size Size of buffer in bytes
 * @return Texture data structure (caller must free with FreeTextureData_C)
 */
ANARI_USD_MIDDLEWARE_C_API CTextureData CreateTextureFromBuffer_C(const unsigned char* buffer,
                                                                   size_t buffer_size);

/**
 * Extract gradient line from image and write as PNG file
 * Specialized function for gradient/colormap processing
 * Extracts the top row of a 2-pixel-high gradient image
 *
 * @param buffer Raw image data containing gradient
 * @param buffer_size Size of buffer in bytes
 * @param output_path Output file path for PNG
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int WriteGradientLineAsPNG_C(const unsigned char* buffer,
                                                        size_t buffer_size,
                                                        const char* output_path);

/**
 * Extract gradient line from image and return PNG data in memory
 * Similar to WriteGradientLineAsPNG_C but returns data instead of writing file
 * Useful for in-memory processing and network transmission
 *
 * @param buffer Raw image data containing gradient
 * @param buffer_size Size of buffer in bytes
 * @param out_png_data Pointer to receive PNG data (caller must free with FreeBuffer_C)
 * @param out_png_size Pointer to receive PNG data size
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int GetGradientLineAsPNGBuffer_C(const unsigned char* buffer,
                                                         size_t buffer_size,
                                                         unsigned char** out_png_data,
                                                         size_t* out_png_size);

/**
 * Extract specific row from image and return PNG data in memory
 * Flexible version of GetGradientLineAsPNGBuffer_C that lets you choose which row to extract
 * Useful for 2-pixel-high gradient images where top row = gradient, bottom row = metadata
 *
 * @param buffer Raw image data containing gradient
 * @param buffer_size Size of buffer in bytes
 * @param row_index Which row to extract (0 = top row, 1 = bottom row for 2-pixel images)
 * @param out_png_data Pointer to receive PNG data (caller must free with FreeBuffer_C)
 * @param out_png_size Pointer to receive PNG data size
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int GetImageRowAsPNGBuffer_C(const unsigned char* buffer,
                                                     size_t buffer_size,
                                                     int row_index,
                                                     unsigned char** out_png_data,
                                                     size_t* out_png_size);

/**
 * Get PNG image dimensions without loading full texture data
 * Lightweight function that reads PNG header to extract width, height, and channels
 * Much faster than CreateTextureFromBuffer_C for just dimension checking
 *
 * @param buffer Raw PNG image data
 * @param buffer_size Size of buffer in bytes
 * @param out_width Pointer to receive image width (pixels)
 * @param out_height Pointer to receive image height (pixels)
 * @param out_channels Pointer to receive number of color channels (3 for RGB, 4 for RGBA)
 * @return 1 on success, 0 on failure (invalid PNG or buffer too small)
 */
ANARI_USD_MIDDLEWARE_C_API int GetPNGDimensions_C(const unsigned char* buffer,
                                               size_t buffer_size,
                                               int* out_width,
                                               int* out_height,
                                               int* out_channels);

// ============================================================================
// MEMORY MANAGEMENT FUNCTIONS
// ============================================================================

/**
 * Free mesh data array allocated by USD loading functions
 * Safely deallocates all internal arrays including collision data
 *
 * IMPORTANT: Always call this function to free mesh data.
 * Do NOT use standard free() or delete[] on mesh arrays.
 *
 * Frees the following arrays for each mesh:
 * - points, indices, normals, uvs, vertex_colors
 * - collision_vertices, collision_indices
 *
 * @param meshes Pointer to mesh array to free
 * @param count Number of meshes in array
 */
ANARI_USD_MIDDLEWARE_C_API void FreeMeshData_C(CMeshData* meshes, size_t count);

/**
 * Free texture data allocated by CreateTextureFromBuffer_C
 *
 * @param texture Pointer to texture data to free
 */
ANARI_USD_MIDDLEWARE_C_API void FreeTextureData_C(CTextureData* texture);

/**
 * Free generic buffer allocated by middleware functions
 * Use this for buffers returned by GetGradientLineAsPNGBuffer_C
 *
 * @param buffer Pointer to buffer to free
 */
ANARI_USD_MIDDLEWARE_C_API void FreeBuffer_C(unsigned char* buffer);

/**
 * Free file data structure (for callback cleanup if needed)
 * Typically not needed as file data is automatically managed
 *
 * @param file_data Pointer to file data to free
 */
ANARI_USD_MIDDLEWARE_C_API void FreeFileData_C(CFileData* file_data);

/**
 * Free frame files array allocated by RequestFrame_C
 *
 * @param files Pointer to frame files array to free
 * @param count Number of files in array
 */
ANARI_USD_MIDDLEWARE_C_API void FreeFrameFiles_C(CFileData* files, size_t count);

// ============================================================================
// CALLBACK REGISTRATION FUNCTIONS
// ============================================================================

/**
 * Register callback function for file reception notifications
 * Only one file callback can be registered at a time
 * Subsequent calls will replace the previous callback
 *
 * The callback is called from the receiver thread context.
 * Keep callback processing minimal to avoid blocking reception.
 *
 * @param callback Function pointer to call when files are received (NULL to unregister)
 */
ANARI_USD_MIDDLEWARE_C_API void RegisterUpdateCallback_C(FileReceivedCallback_C callback);

/**
 * Register callback function for message reception notifications
 * Only one message callback can be registered at a time
 * Subsequent calls will replace the previous callback
 *
 * The callback is called from the receiver thread context.
 * Keep callback processing minimal to avoid blocking reception.
 *
 * @param callback Function pointer to call when messages are received (NULL to unregister)
 */
ANARI_USD_MIDDLEWARE_C_API void RegisterMessageCallback_C(MessageReceivedCallback_C callback);

// ============================================================================
// UTILITY AND DEBUG FUNCTIONS
// ============================================================================

/**
 * Get middleware version information
 * Returns version string with build information
 *
 * @return Pointer to version string (static, always valid)
 */
ANARI_USD_MIDDLEWARE_C_API const char* GetMiddlewareVersion_C(void);

/**
 * Validate USD file format without full processing
 * Quick check to determine if buffer contains valid USD data
 *
 * @param buffer USD data buffer to validate
 * @param buffer_size Size of buffer in bytes
 * @param filename Filename for format detection
 * @return 1 if valid USD format, 0 if invalid
 */
ANARI_USD_MIDDLEWARE_C_API int ValidateUSDFormat_C(const unsigned char* buffer,
                                                   size_t buffer_size,
                                                   const char* filename);

/**
 * Get supported USD file extensions
 * Returns comma-separated list of supported extensions
 *
 * @return Pointer to extension list string (static, always valid)
 */
ANARI_USD_MIDDLEWARE_C_API const char* GetSupportedUSDExtensions_C(void);

/**
 * Reset processing statistics
 * Clears all internal counters and statistics
 * Useful for performance monitoring and testing
 */
ANARI_USD_MIDDLEWARE_C_API void ResetProcessingStats_C(void);

/**
 * Get processing statistics as formatted string
 * Returns detailed information about processed files, meshes, errors, etc.
 *
 * @return Pointer to statistics string (valid until next call)
 */
ANARI_USD_MIDDLEWARE_C_API const char* GetProcessingStats_C(void);

// ============================================================================
// BROKER CONNECTION AND FILE REQUEST FUNCTIONS
// ============================================================================

/**
 * Connect to ANARI USD broker as DEALER client
 *
 * @param broker_endpoint Broker endpoint (e.g., "tcp://localhost:5555")
 * @param timeout_ms Connection timeout in milliseconds
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int ConnectToBroker_C(
    const char* broker_endpoint,
    int timeout_ms);

/**
 * Disconnect from broker
 */
ANARI_USD_MIDDLEWARE_C_API void DisconnectFromBroker_C(void);

/**
 * Check if connected to broker
 *
 * @return 1 if connected, 0 if not connected
 */
ANARI_USD_MIDDLEWARE_C_API int IsBrokerConnected_C(void);

/**
 * Request file list from specific worker rank
 *
 * @param target_rank Target worker rank
 * @param out_files Pointer to receive array of filenames (caller must free with FreeFileList_C)
 * @param out_count Pointer to receive number of files
 * @param timeout_ms Timeout in milliseconds
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int RequestFileList_C(
    int32_t target_rank,
    char*** out_files,
    size_t* out_count,
    int timeout_ms);

/**
 * Request file list with sizes from worker rank
 *
 * @param target_rank Target worker rank
 * @param out_names Pointer to receive array of filenames (caller must free with FreeFileList_C)
 * @param out_sizes Pointer to receive array of file sizes (caller must free with FreeBuffer_C)
 * @param out_count Pointer to receive number of files
 * @param timeout_ms Timeout in milliseconds
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int RequestFileListWithSizes_C(
    int32_t target_rank,
    char*** out_names,
    uint64_t** out_sizes,
    size_t* out_count,
    int timeout_ms);

/**
 * Request file list with sizes and source ranks from worker rank(s)
 * When target_rank = -1 (broadcast), returns files from all ranks with their source ranks
 *
 * @param target_rank Target worker rank (-1 for broadcast to all ranks)
 * @param out_names Pointer to receive array of filenames (caller must free with FreeFileList_C)
 * @param out_sizes Pointer to receive array of file sizes (caller must free with FreeBuffer_C)
 * @param out_ranks Pointer to receive array of source ranks (caller must free with FreeBuffer_C)
 * @param out_count Pointer to receive number of files
 * @param timeout_ms Timeout in milliseconds
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int RequestFileListWithSizesAndRanks_C(
    int32_t target_rank,
    char*** out_names,
    uint64_t** out_sizes,
    int32_t** out_ranks,
    size_t* out_count,
    int timeout_ms);

/**
 * Free file list allocated by RequestFileList_C
 *
 * @param files Array of filenames to free
 * @param count Number of files in array
 */
ANARI_USD_MIDDLEWARE_C_API void FreeFileList_C(
    char** files,
    size_t count);

/**
 * Free file list with sizes allocated by RequestFileListWithSizes_C
 *
 * @param names Array of filenames to free
 * @param sizes Array of file sizes to free
 * @param count Number of files in array
 */
ANARI_USD_MIDDLEWARE_C_API void FreeFileListWithSizes_C(
    char** names,
    uint64_t* sizes,
    size_t count);

/**
 * Free file list with sizes and ranks allocated by RequestFileListWithSizesAndRanks_C
 *
 * @param names Array of filenames to free
 * @param sizes Array of file sizes to free
 * @param ranks Array of source ranks to free
 * @param count Number of files in array
 */
ANARI_USD_MIDDLEWARE_C_API void FreeFileListWithSizesAndRanks_C(
    char** names,
    uint64_t* sizes,
    int32_t* ranks,
    size_t count);

/**
 * Request specific file from worker rank
 *
 * @param filename Name of file to request
 * @param target_rank Target worker rank
 * @param out_data Pointer to receive file data (caller must free with FreeBuffer_C)
 * @param out_size Pointer to receive data size
 * @param timeout_ms Timeout in milliseconds
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int RequestFile_C(
    const char* filename,
    int32_t target_rank,
    unsigned char** out_data,
    size_t* out_size,
    int timeout_ms);

/**
 * Request frame (collection of files) from worker rank
 *
 * @param frame_number Frame number to request
 * @param target_rank Target worker rank
 * @param out_files Pointer to receive array of file data (caller must free with FreeFileData_C for each)
 * @param out_count Pointer to receive number of files in frame
 * @param timeout_ms Timeout in milliseconds
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int RequestFrame_C(
    int32_t frame_number,
    int32_t target_rank,
    CFileData** out_files,
    size_t* out_count,
    int timeout_ms);

/**
 * Request worker count excluding rank 0 (computational workers only)
 * Uses binary protocol REQ_WORKER_COUNT/RESP_WORKER_COUNT
 *
 * @param out_count Pointer to receive worker count (excluding rank 0)
 * @param timeout_ms Timeout in milliseconds
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int RequestWorkerCountExcludingRank0_C(
    uint32_t* out_count,
    int timeout_ms);

// ============================================================================
// WORKER LIST / COUNT FUNCTIONS (Legacy String Protocol)
// ============================================================================

/**
 * Request worker list from broker using legacy string protocol
 * Returns list of all workers including rank 0
 * Format: "rank:hostname:ip;rank:hostname:ip;..."
 *
 * @param out_worker_count Pointer to receive number of workers
 * @param out_data Buffer to receive worker list data (caller must free with FreeBuffer_C)
 * @param out_size Pointer to receive data size
 * @param timeout_ms Timeout in milliseconds
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int RequestWorkerListString_C(
    uint32_t* out_worker_count,
    unsigned char** out_data,
    size_t* out_size,
    int timeout_ms);

/**
 * Get total worker count including rank 0
 * Wrapper around RequestWorkerListString_C that just returns the count
 *
 * @param out_total_count Pointer to receive total worker count
 * @param timeout_ms Timeout in milliseconds
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int RequestTotalWorkerCount_C(
    uint32_t* out_total_count,
    int timeout_ms);

// ============================================================================
// ASYNC BROKER FUNCTIONS (NON-BLOCKING)
// ============================================================================

/**
 * Callback types for async broker operations
 */
typedef void (*WorkerCountCallback_C)(uint32_t worker_count);
typedef void (*WorkerStatusCallback_C)(int32_t target_rank, const char* status_data, size_t status_size);
typedef void (*FileListCallback_C)(int32_t target_rank, char** files, size_t file_count);
typedef void (*BrokerErrorCallback_C)(const char* error_message);

/**
 * Request total worker count asynchronously (non-blocking)
 * Calls callback on background thread when complete
 *
 * @param callback Function to call with worker count on success
 * @param error_callback Function to call on error (can be NULL)
 * @param timeout_ms Timeout in milliseconds
 */
ANARI_USD_MIDDLEWARE_C_API void RequestTotalWorkerCountAsync_C(
    WorkerCountCallback_C callback,
    BrokerErrorCallback_C error_callback,
    int timeout_ms);

/**
 * Request worker count asynchronously (non-blocking)
 * Calls callback on background thread when complete
 *
 * @param callback Function to call with worker count on success
 * @param error_callback Function to call on error (can be NULL)
 * @param timeout_ms Timeout in milliseconds
 */
ANARI_USD_MIDDLEWARE_C_API void RequestWorkerCountAsync_C(
    WorkerCountCallback_C callback,
    BrokerErrorCallback_C error_callback,
    int timeout_ms);

/**
 * Request worker status asynchronously (non-blocking)
 * Calls callback on background thread when complete
 *
 * @param target_rank Target worker rank (-1 for all workers)
 * @param callback Function to call with worker status on success
 * @param error_callback Function to call on error (can be NULL)
 * @param timeout_ms Timeout in milliseconds
 */
ANARI_USD_MIDDLEWARE_C_API void RequestWorkerStatusAsync_C(
    int32_t target_rank,
    WorkerStatusCallback_C callback,
    BrokerErrorCallback_C error_callback,
    int timeout_ms);

/**
 * Request file list asynchronously (non-blocking)
 * Calls callback on background thread when complete
 *
 * @param target_rank Target worker rank
 * @param callback Function to call with file list on success
 * @param error_callback Function to call on error (can be NULL)
 * @param timeout_ms Timeout in milliseconds
 */
ANARI_USD_MIDDLEWARE_C_API void RequestFileListAsync_C(
    int32_t target_rank,
    FileListCallback_C callback,
    BrokerErrorCallback_C error_callback,
    int timeout_ms);

// ========== SCENE SNAPSHOT SUPPORT ==========

/**
 * Callback for scene snapshot JSON response
 * Called when broker returns full scene state
 *
 * @param json_string JSON string containing scene snapshot:
 *   {
 *     "request_id": ...,
 *     "type": "scene_snapshot",
 *     "timestamp": ...,
 *     "num_workers": ...,
 *     "workers": [{"rank": ..., "hostname": ..., "ready": true/false}, ...],
 *     "files": [{"name": ..., "size": ..., "hash_lo": ..., "hash_hi": ..., "mime": ...}, ...]
 *   }
 */
typedef void (*SceneSnapshotCallback_C)(const char* json_string);

/**
 * Request full scene snapshot from broker (non-blocking)
 * Returns all registered workers and rank 0 memory store files with hashes
 *
 * @param callback Called with JSON scene snapshot on success
 * @param error_callback Called on error (can be NULL)
 * @param timeout_ms Timeout in milliseconds
 */
ANARI_USD_MIDDLEWARE_C_API void RequestSceneSnapshotAsync_C(
    SceneSnapshotCallback_C callback,
    BrokerErrorCallback_C error_callback,
    int timeout_ms);

// ========== PARALLEL FILE DOWNLOAD SUPPORT ==========

/**
 * Callback for individual file received during parallel download
 * Called immediately when each file completes download
 *
 * @param filename Name of the file that was downloaded
 * @param data Binary file data
 * @param data_size Size of data in bytes
 */
typedef void (*ParallelFileReceivedCallback_C)(const char* filename, const unsigned char* data, size_t data_size);

/**
 * Callback for parallel download completion
 * Called when ALL files in a parallel download batch are complete
 */
typedef void (*ParallelDownloadCompleteCallback_C)(void);

/**
 * Callback for parallel download errors (per-file)
 * Called when a specific file fails to download
 *
 * @param filename Name of the file that failed
 * @param error_message Error description
 */
typedef void (*ParallelDownloadErrorCallback_C)(const char* filename, const char* error_message);

/**
 * Request multiple files in parallel (non-blocking)
 * Downloads files simultaneously with RAM awareness and immediate spawning
 * Uses single DEALER socket with client-side multiplexing
 *
 * @param filenames Array of filenames to download
 * @param filename_count Number of filenames in array
 * @param target_ranks Array of target ranks (must match filename_count)
 * @param file_received_callback Called immediately for each file as it downloads
 * @param completion_callback Called when ALL files are complete (can be NULL)
 * @param error_callback Called for each file that fails (can be NULL)
 * @param timeout_ms Timeout in milliseconds
 */
ANARI_USD_MIDDLEWARE_C_API void RequestFilesParallelAsync_C(
    const char** filenames,
    size_t filename_count,
    const int32_t* target_ranks,
    ParallelFileReceivedCallback_C file_received_callback,
    ParallelDownloadCompleteCallback_C completion_callback,
    ParallelDownloadErrorCallback_C error_callback,
    int timeout_ms);

/**
 * Version verification function - call this from Unreal to verify DLL is loaded correctly
 * Returns: 1 if working, 0 if broken
 */
ANARI_USD_MIDDLEWARE_C_API int VerifyParallelDownloadDLL_C();

/**
 * Direct C API for parallel downloads (synchronous version)
 * Downloads multiple files in parallel and returns results through callbacks
 * This is a more direct wrapper that avoids C++ async complexities
 * 
 * @param filenames Array of filename strings
 * @param filename_count Number of filenames
 * @param target_ranks Array of target ranks (parallel to filenames)
 * @param file_received_callback Called for each file received (can be NULL)
 * @param completion_callback Called when all downloads complete (can be NULL)
 * @param error_callback Called for each file that fails (can be NULL)
 * @param timeout_ms Timeout in milliseconds
 * @return 1 if download started successfully, 0 if failed
 */
ANARI_USD_MIDDLEWARE_C_API int RequestFilesParallelDirect_C(
    const char** filenames,
    size_t filename_count,
    const int32_t* target_ranks,
    ParallelFileReceivedCallback_C file_received_callback,
    ParallelDownloadCompleteCallback_C completion_callback,
    ParallelDownloadErrorCallback_C error_callback,
    int timeout_ms);

// ============================================================================
// MESH ACCELERATOR FUNCTIONS (GPU/CPU ACCELERATION)
// ============================================================================

/**
 * Create and initialize MeshAccelerator instance
 * Provides GPU (CUDA) and CPU (AVX-512) accelerated mesh processing
 *
 * @return Opaque handle to MeshAccelerator instance (NULL on failure)
 */
ANARI_USD_MIDDLEWARE_C_API void* CreateMeshAccelerator_C(void);

/**
 * Configure MeshAccelerator with specific settings
 *
 * @param accelerator Handle returned by CreateMeshAccelerator_C
 * @param preferred_backend 0=AUTO, 1=CUDA, 2=AVX512, 3=AVX2, 4=SSE4, 5=SCALAR
 * @param min_vertices_for_gpu Minimum vertices to use GPU acceleration
 * @param enable_async Enable asynchronous processing
 * @param memory_pool_size_mb Memory pool size in MB (0 to disable)
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int ConfigureMeshAccelerator_C(void* accelerator,
                                                         int preferred_backend,
                                                         size_t min_vertices_for_gpu,
                                                         int enable_async,
                                                         size_t memory_pool_size_mb);

/**
 * Transform vertices using GPU/CPU acceleration
 * Applies world transformation matrix to vertex positions
 *
 * @param accelerator Handle returned by CreateMeshAccelerator_C
 * @param vertices Array of vertex positions [x1,y1,z1, x2,y2,z2, ...]
 * @param vertex_count Number of vertices (not floats)
 * @param world_transform 4x4 transformation matrix (16 floats, row-major)
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int TransformVerticesAccelerated_C(void* accelerator,
                                                            float* vertices,
                                                            size_t vertex_count,
                                                            const float* world_transform);

/**
 * Calculate normals using GPU/CPU acceleration
 * Computes vertex normals from triangle indices
 *
 * @param accelerator Handle returned by CreateMeshAccelerator_C
 * @param vertices Array of vertex positions [x1,y1,z1, x2,y2,z2, ...]
 * @param vertex_count Number of vertices (not floats)
 * @param indices Array of triangle indices [i1,i2,i3, i4,i5,i6, ...]
 * @param index_count Number of indices (must be multiple of 3)
 * @param normals Output array for normals (must be pre-allocated with vertex_count * 3 floats)
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int CalculateNormalsAccelerated_C(void* accelerator,
                                                           const float* vertices,
                                                           size_t vertex_count,
                                                           const unsigned int* indices,
                                                           size_t index_count,
                                                           float* normals);

/**
 * Transform normals using GPU/CPU acceleration
 * Applies normal transformation matrix to normals
 *
 * @param accelerator Handle returned by CreateMeshAccelerator_C
 * @param normals Array of normals [nx1,ny1,nz1, nx2,ny2,nz2, ...]
 * @param normal_count Number of normals (not floats)
 * @param normal_matrix 3x3 normal transformation matrix (9 floats, row-major)
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int TransformNormalsAccelerated_C(void* accelerator,
                                                           float* normals,
                                                           size_t normal_count,
                                                           const float* normal_matrix);

/**
 * Get performance metrics from last operation
 *
 * @param accelerator Handle returned by CreateMeshAccelerator_C
 * @param out_vertices_processed Pointer to receive vertices processed count
 * @param out_processing_time_ms Pointer to receive processing time in milliseconds
 * @param out_throughput_vertices_per_sec Pointer to receive throughput
 * @param out_used_backend Pointer to receive backend used (0=CUDA, 1=AVX512, etc.)
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int GetMeshAcceleratorMetrics_C(void* accelerator,
                                                         size_t* out_vertices_processed,
                                                         double* out_processing_time_ms,
                                                         double* out_throughput_vertices_per_sec,
                                                         int* out_used_backend);

/**
 * Get system information about available acceleration backends
 *
 * @param out_has_cuda Pointer to receive 1 if CUDA available, 0 otherwise
 * @param out_has_avx512 Pointer to receive 1 if AVX-512 available, 0 otherwise
 * @param out_has_avx2 Pointer to receive 1 if AVX2 available, 0 otherwise
 * @param out_has_sse4 Pointer to receive 1 if SSE4 available, 0 otherwise
 * @param out_cpu_cores Pointer to receive number of CPU cores
 * @param out_cuda_device_count Pointer to receive number of CUDA devices
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int GetAccelerationSystemInfo_C(int* out_has_cuda,
                                                         int* out_has_avx512,
                                                         int* out_has_avx2,
                                                         int* out_has_sse4,
                                                         int* out_cpu_cores,
                                                         int* out_cuda_device_count);

/**
 * Destroy MeshAccelerator instance and free resources
 *
 * @param accelerator Handle returned by CreateMeshAccelerator_C
 */
ANARI_USD_MIDDLEWARE_C_API void DestroyMeshAccelerator_C(void* accelerator);

// ============================================================================
// PERFORMANCE OPTIMIZATION FUNCTIONS
// ============================================================================

/**
 * Initialize thread pool for parallel USD processing
 * Call this before using any USD loading functions for optimal performance
 *
 * @param thread_count Number of threads to use (0 = auto-detect based on CPU cores)
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int InitializeThreadPool_C(int thread_count);

/**
 * Get thread pool statistics
 *
 * @param out_thread_count Pointer to receive number of threads in pool
 * @param out_pending_tasks Pointer to receive number of pending tasks
 * @param out_active_tasks Pointer to receive number of active tasks
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int GetThreadPoolStats_C(int* out_thread_count, 
                                                   int* out_pending_tasks,
                                                   int* out_active_tasks);

/**
 * Enable/disable memory pooling for mesh data
 * When enabled, mesh allocations use a memory pool for better performance
 *
 * @param enable 1 to enable, 0 to disable
 * @param pool_size_mb Initial memory pool size in MB (default: 16MB)
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int SetMemoryPooling_C(int enable, int pool_size_mb);

/**
 * Get memory pool statistics
 *
 * @param out_current_usage Pointer to receive current memory usage in bytes
 * @param out_total_allocated Pointer to receive total allocated bytes
 * @param out_peak_usage Pointer to receive peak memory usage in bytes
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int GetMemoryPoolStats_C(size_t* out_current_usage,
                                                   size_t* out_total_allocated,
                                                   size_t* out_peak_usage);

/**
 * Process USD file with streaming callback (for large files)
 * Processes the USD file in chunks and calls callback for each batch of meshes
 *
 * @param filepath Path to USD file
 * @param callback Callback function for processed mesh batches
 * @param user_data User data passed to callback
 * @param batch_size Number of meshes to process per batch (default: 10)
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int ProcessUSDStreaming_C(const char* filepath,
                                                    void (*callback)(CMeshData* meshes, size_t count, void* user_data),
                                                    void* user_data,
                                                    int batch_size);

/**
 * Split vertices on GPU (for meshes with shared vertices but different normals/UVs)
 * Creates duplicate vertices so each triangle has unique vertices
 *
 * @param mesh_data Mesh data to split
 * @param out_vertex_count Pointer to receive new vertex count after splitting
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int SplitVerticesGPU_C(CMeshData* mesh_data, size_t* out_vertex_count);

/**
 * Weld vertices on GPU (merge duplicate vertices)
 * Reduces vertex count by merging vertices that are close together
 *
 * @param mesh_data Mesh data to weld
 * @param position_epsilon Maximum distance for vertices to be considered identical
 * @param normal_epsilon Maximum angle difference for normals to be considered identical
 * @param uv_epsilon Maximum UV coordinate difference
 * @param out_vertex_count Pointer to receive new vertex count after welding
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int WeldVerticesGPU_C(CMeshData* mesh_data,
                                                float position_epsilon,
                                                float normal_epsilon,
                                                float uv_epsilon,
                                                size_t* out_vertex_count);

// ============================================================================
// OPTIMIZATION STATISTICS STRUCTURES
// ============================================================================

/**
 * Thread pool statistics structure
 * Used by GetOptimizationStats_C to report thread pool performance
 */
typedef struct {
    int enabled;                    // Whether thread pool is enabled (1) or disabled (0)
    int max_threads;                // Maximum number of worker threads
    int active_threads;             // Number of currently active threads
    int idle_threads;               // Number of idle threads
    int queued_tasks;               // Number of tasks in queue
    float avg_task_time_ms;         // Average task execution time in milliseconds
    float max_task_time_ms;         // Maximum task execution time in milliseconds
    float min_task_time_ms;         // Minimum task execution time in milliseconds
    int total_tasks_processed;      // Total tasks processed since start
    int failed_tasks;               // Number of failed tasks
    float tasks_per_second;         // Tasks processed per second
    float cpu_utilization_percent;  // CPU utilization percentage (0-100)
    size_t thread_stack_memory_bytes; // Memory used for thread stacks
    size_t task_queue_memory_bytes; // Memory used for task queue
} AnariUsdThreadPoolStats;

/**
 * Memory pool statistics structure
 * Used by GetOptimizationStats_C to report memory pool performance
 */
typedef struct {
    int enabled;                    // Whether memory pool is enabled (1) or disabled (0)
    size_t block_size_bytes;        // Size of each memory block in bytes
    int total_blocks;               // Total number of blocks
    int free_blocks;                // Currently free blocks
    int used_blocks;                // Currently used blocks
    size_t total_memory_bytes;      // Total memory allocated in bytes
    size_t used_memory_bytes;       // Currently used memory in bytes
    size_t free_memory_bytes;       // Currently free memory in bytes
    size_t peak_memory_bytes;       // Peak memory usage in bytes
    int allocation_count;           // Total number of allocations
    int deallocation_count;         // Total number of deallocations
    float avg_allocation_time_ms;   // Average allocation time in milliseconds
    float avg_deallocation_time_ms; // Average deallocation time in milliseconds
    float fragmentation_percent;    // Memory fragmentation percentage (0-100)
    float utilization_percent;      // Memory utilization percentage (0-100)
    int cache_hits;                 // Number of cache hits
    int cache_misses;               // Number of cache misses
    float cache_hit_rate_percent;   // Cache hit rate percentage (0-100)
} AnariUsdMemoryPoolStats;

/**
 * GPU acceleration statistics structure
 * Used by GetOptimizationStats_C to report GPU performance
 */
typedef struct {
    int enabled;                    // Whether GPU acceleration is enabled (1) or disabled (0)
    char gpu_name[256];             // GPU device name
    int compute_capability_major;   // CUDA compute capability major version
    int compute_capability_minor;   // CUDA compute capability minor version
    size_t total_vram_bytes;        // Total GPU memory in bytes
    size_t free_vram_bytes;         // Free GPU memory in bytes
    float vertex_splitting_kernel_time_ms; // Vertex splitting kernel time
    float vertex_welding_kernel_time_ms;   // Vertex welding kernel time
    float normal_calculation_kernel_time_ms; // Normal calculation kernel time
    float uv_processing_kernel_time_ms;    // UV processing kernel time
    float vertices_processed_per_second;   // Vertices processed per second
    float triangles_processed_per_second;  // Triangles processed per second
    float gpu_utilization_percent;  // GPU utilization percentage (0-100)
    float host_to_device_bandwidth_gbps; // Host to device bandwidth
    float device_to_host_bandwidth_gbps; // Device to host bandwidth
    size_t total_data_transferred_bytes; // Total data transferred
    int kernel_launches;            // Total kernel launches
    int concurrent_kernels;         // Maximum concurrent kernels
    float avg_kernel_launch_overhead_ms; // Average kernel launch overhead
} AnariUsdGPUStats;

/**
 * Streaming processing statistics structure
 * Used by GetOptimizationStats_C to report streaming performance
 */
typedef struct {
    int enabled;                    // Whether streaming is enabled (1) or disabled (0)
    size_t chunk_size_bytes;        // Chunk size in bytes
    int max_concurrent_chunks;      // Maximum concurrent chunks
    int prefetch_buffer_size;       // Prefetch buffer size
    float avg_chunk_load_time_ms;   // Average chunk load time in milliseconds
    float max_chunk_load_time_ms;   // Maximum chunk load time in milliseconds
    float min_chunk_load_time_ms;   // Minimum chunk load time in milliseconds
    float data_throughput_mbps;     // Data throughput in MB/s
    int total_chunks_processed;     // Total chunks processed
    int failed_chunks;              // Number of failed chunks
    size_t chunk_cache_memory_bytes; // Memory used for chunk cache
    size_t active_chunks_memory_bytes; // Memory used for active chunks
    float cache_hit_rate_percent;   // Cache hit rate percentage (0-100)
    int load_queue_size;            // Size of load queue
    int process_queue_size;         // Size of process queue
    int ready_queue_size;           // Size of ready queue
} AnariUsdStreamingStats;

// ============================================================================
// OPTIMIZATION CONFIGURATION FUNCTIONS (Unreal-compatible)
// ============================================================================

/**
 * Configure all optimization settings at once
 * Comprehensive configuration for thread pool, memory pool, GPU acceleration, and streaming
 *
 * @param enable_thread_pool Enable thread pool (1) or disable (0)
 * @param thread_pool_size Number of worker threads (0 = auto-detect)
 * @param max_queue_size Maximum queue size for pending tasks
 * @param enable_memory_pool Enable memory pool (1) or disable (0)
 * @param memory_pool_block_size Size of each memory block in bytes
 * @param memory_pool_max_blocks Maximum number of memory blocks
 * @param enable_gpu_acceleration Enable GPU acceleration (1) or disable (0)
 * @param gpu_device_id GPU device ID (0 = default device)
 * @param gpu_threads_per_block CUDA threads per block
 * @param gpu_blocks_per_grid CUDA blocks per grid
 * @param enable_streaming Enable streaming processing (1) or disable (0)
 * @param streaming_chunk_size Chunk size for streaming in bytes
 * @param max_concurrent_chunks Maximum concurrent chunks to process
 * @return 0 on success, non-zero error code on failure
 */
ANARI_USD_MIDDLEWARE_C_API int anari_usd_configure_optimizations(
    int enable_thread_pool,
    int thread_pool_size,
    int max_queue_size,
    int enable_memory_pool,
    size_t memory_pool_block_size,
    int memory_pool_max_blocks,
    int enable_gpu_acceleration,
    int gpu_device_id,
    int gpu_threads_per_block,
    int gpu_blocks_per_grid,
    int enable_streaming,
    size_t streaming_chunk_size,
    int max_concurrent_chunks
);

/**
 * Enable or disable thread pool
 *
 * @param enabled Enable (1) or disable (0)
 * @param thread_count Number of threads (0 = auto-detect)
 * @return 0 on success, non-zero error code on failure
 */
ANARI_USD_MIDDLEWARE_C_API int anari_usd_set_thread_pool_enabled(int enabled, int thread_count);

/**
 * Enable or disable memory pool
 *
 * @param enabled Enable (1) or disable (0)
 * @param block_size Block size in bytes
 * @param max_blocks Maximum number of blocks
 * @return 0 on success, non-zero error code on failure
 */
ANARI_USD_MIDDLEWARE_C_API int anari_usd_set_memory_pool_enabled(int enabled, size_t block_size, int max_blocks);

/**
 * Enable or disable GPU acceleration
 *
 * @param enabled Enable (1) or disable (0)
 * @param device_id GPU device ID (0 = default device)
 * @return 0 on success, non-zero error code on failure
 */
ANARI_USD_MIDDLEWARE_C_API int anari_usd_set_gpu_acceleration_enabled(int enabled, int device_id);

/**
 * Enable or disable streaming processing
 *
 * @param enabled Enable (1) or disable (0)
 * @param chunk_size Chunk size in bytes
 * @param max_concurrent_chunks Maximum concurrent chunks
 * @return 0 on success, non-zero error code on failure
 */
ANARI_USD_MIDDLEWARE_C_API int anari_usd_set_streaming_enabled(int enabled, size_t chunk_size, int max_concurrent_chunks);

/**
 * Get detailed optimization statistics
 * Retrieves statistics for thread pool, memory pool, GPU acceleration, and streaming
 *
 * @param out_thread_pool_stats Pointer to receive thread pool statistics
 * @param out_memory_pool_stats Pointer to receive memory pool statistics
 * @param out_gpu_stats Pointer to receive GPU statistics
 * @param out_streaming_stats Pointer to receive streaming statistics
 * @return 0 on success, non-zero error code on failure
 */
ANARI_USD_MIDDLEWARE_C_API int anari_usd_get_optimization_stats(
    AnariUsdThreadPoolStats* out_thread_pool_stats,
    AnariUsdMemoryPoolStats* out_memory_pool_stats,
    AnariUsdGPUStats* out_gpu_stats,
    AnariUsdStreamingStats* out_streaming_stats
);

/**
 * Get overall optimization statistics as a single structure
 * Simplified version for Unreal Blueprint integration
 *
 * @param out_overall_stats Pointer to receive overall statistics structure
 * @return 0 on success, non-zero error code on failure
 */
ANARI_USD_MIDDLEWARE_C_API int anari_usd_get_overall_optimization_stats(void* out_overall_stats);

/**
 * Reset all optimization statistics to zero
 * Useful for benchmarking and performance testing
 *
 * @return 0 on success, non-zero error code on failure
 */
ANARI_USD_MIDDLEWARE_C_API int anari_usd_reset_optimization_stats(void);

/**
 * Load USD data with optimization settings applied
 * Enhanced version that uses all configured optimizations
 *
 * @param buffer Raw USD file data
 * @param buffer_size Size of buffer in bytes
 * @param filename Original filename (used for format detection)
 * @param out_meshes Pointer to receive array of extracted meshes (caller must free)
 * @param out_count Pointer to receive number of extracted meshes
 * @return 0 on success, non-zero error code on failure
 */
ANARI_USD_MIDDLEWARE_C_API int anari_usd_load_with_optimizations(
    const unsigned char* buffer,
    size_t buffer_size,
    const char* filename,
    void** out_meshes,
    size_t* out_count
);

/**
 * Get performance optimization settings
 *
 * @param out_use_thread_pool Pointer to receive thread pool enabled status
 * @param out_use_memory_pool Pointer to receive memory pool enabled status
 * @param out_use_gpu_accel Pointer to receive GPU acceleration enabled status
 * @param out_max_threads Pointer to receive maximum thread count
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int GetOptimizationSettings_C(int* out_use_thread_pool,
                                                        int* out_use_memory_pool,
                                                        int* out_use_gpu_accel,
                                                        int* out_max_threads);

/**
 * Set performance optimization settings
 *
 * @param use_thread_pool Enable thread pool (1) or disable (0)
 * @param use_memory_pool Enable memory pool (1) or disable (0)
 * @param use_gpu_accel Enable GPU acceleration (1) or disable (0)
 * @param max_threads Maximum number of threads to use (0 = auto-detect)
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int SetOptimizationSettings_C(int use_thread_pool,
                                                        int use_memory_pool,
                                                        int use_gpu_accel,
                                                        int max_threads);

#ifdef __cplusplus
}
#endif

#endif // ANARI_USD_MIDDLEWARE_C_H
