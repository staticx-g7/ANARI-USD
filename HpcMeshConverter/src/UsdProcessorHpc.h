#pragma once

#include <vector>
#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <functional>
#include <glm/glm.hpp>
#include "MiddlewareLogging.h"
#include "../ThirdParty/include/CollisionProcessor.h"

namespace anari_usd_middleware {
    struct MeshData;
}

using namespace anari_usd_middleware;

class UsdProcessorHpc {
public:
    struct MeshData {
        std::string elementName;        ///< Name of the USD element (validated)
        std::string typeName;           ///< Type of the USD element (validated)
        std::vector<glm::vec3> points;  ///< 3D vertex positions (size-limited)
        std::vector<uint32_t> indices;  ///< Triangle indices (validated)
        std::vector<glm::vec3> normals; ///< Normal vectors (normalized)
        std::vector<glm::vec2> uvs;     ///< Texture coordinates (clamped)
        std::vector<glm::vec4> vertex_colors; ///< Vertex colors

        std::string subdivisionScheme;
        bool doubleSided = false;       ///< Double-sided flag from USD
        std::vector<uint32_t> faceVertexCounts;  ///< Face vertex counts for heterogenous polygons
        std::vector<std::vector<glm::vec2>> uvSets;  ///< Multiple UV sets (channels)
        std::vector<std::string> uvSetNames;  ///< Names of UV sets

        bool isValid() const {
            return !elementName.empty() &&
                   points.size() <= safety::MAX_MESH_VERTICES &&
                   indices.size() <= safety::MAX_MESH_INDICES &&
                   normals.size() <= safety::MAX_MESH_VERTICES &&
                   uvs.size() <= safety::MAX_MESH_VERTICES &&
                   (indices.size() % 3 == 0) &&
                   validateGeometry();
        }

        size_t getVertexCount() const { return points.size(); }
        size_t getTriangleCount() const { return indices.size() / 3; }
        bool hasNormals() const { return !normals.empty(); }
        bool hasUVs() const { return !uvs.empty(); }
        bool hasSubdivision() const { return !subdivisionScheme.empty() && subdivisionScheme != "none"; }
        size_t getUVSetCount() const { return uvSets.size(); }

        std::pair<glm::vec3, glm::vec3> getBounds() const;

        bool validateGeometry() const;

        void clear() {
            elementName.clear();
            typeName.clear();
            points.clear();
            indices.clear();
            normals.clear();
            uvs.clear();
            vertex_colors.clear();
            subdivisionScheme.clear();
            doubleSided = false;
            faceVertexCounts.clear();
            uvSets.clear();
            uvSetNames.clear();
        }

        anari_usd_middleware::MeshData toMiddlewareMeshData() const;
    };

    struct ProcessingStats {
        std::atomic<uint64_t> filesProcessed{0};
        std::atomic<uint64_t> meshesExtracted{0};
        std::atomic<uint64_t> texturesProcessed{0};
        std::atomic<uint64_t> referencesResolved{0};
        std::atomic<uint64_t> processingErrors{0};
        std::atomic<uint64_t> totalBytesProcessed{0};

        ProcessingStats() = default;
        ProcessingStats(const ProcessingStats&) = delete;
        ProcessingStats& operator=(const ProcessingStats&) = delete;

        ProcessingStats(ProcessingStats&& other) noexcept {
            filesProcessed.store(other.filesProcessed.load());
            meshesExtracted.store(other.meshesExtracted.load());
            texturesProcessed.store(other.texturesProcessed.load());
            referencesResolved.store(other.referencesResolved.load());
            processingErrors.store(other.processingErrors.load());
            totalBytesProcessed.store(other.totalBytesProcessed.load());
        }

        ProcessingStats& operator=(ProcessingStats&& other) noexcept {
            if (this != &other) {
                filesProcessed.store(other.filesProcessed.load());
                meshesExtracted.store(other.meshesExtracted.load());
                texturesProcessed.store(other.texturesProcessed.load());
                referencesResolved.store(other.referencesResolved.load());
                processingErrors.store(other.processingErrors.load());
                totalBytesProcessed.store(other.totalBytesProcessed.load());
            }
            return *this;
        }

        void reset() {
            filesProcessed.store(0);
            meshesExtracted.store(0);
            texturesProcessed.store(0);
            referencesResolved.store(0);
            processingErrors.store(0);
            totalBytesProcessed.store(0);
        }

        struct Snapshot {
            uint64_t filesProcessed;
            uint64_t meshesExtracted;
            uint64_t texturesProcessed;
            uint64_t referencesResolved;
            uint64_t processingErrors;
            uint64_t totalBytesProcessed;
        };

        Snapshot getSnapshot() const {
            return {
                filesProcessed.load(),
                meshesExtracted.load(),
                texturesProcessed.load(),
                referencesResolved.load(),
                processingErrors.load(),
                totalBytesProcessed.load()
            };
        }
    };

    using ProgressCallback = std::function<void(float progress, const std::string& status)>;

    UsdProcessorHpc();

    ~UsdProcessorHpc();

    UsdProcessorHpc(const UsdProcessorHpc&) = delete;
    UsdProcessorHpc& operator=(const UsdProcessorHpc&) = delete;

    bool initialize(const std::string& hpcEndpoint, int numThreads);

    bool processUSDFile(const std::string& inputPath, const std::string& outputPath,
                       ECollisionComplexity complexity);

    bool extractMeshes(const std::string& inputPath, const std::string& outputPath);

    void shutdown();

    bool LoadUSDBuffer(const std::vector<uint8_t>& buffer,
                      const std::string& fileName,
                      std::vector<MeshData>& outMeshData,
                      ProgressCallback progressCallback = nullptr);

    bool LoadUSDFromDisk(const std::string& filePath,
                        std::vector<MeshData>& outMeshData,
                        ProgressCallback progressCallback = nullptr);

    ProcessingStats::Snapshot getProcessingStats() const;

    void resetProcessingStats();

    bool validateUSDFormat(const std::vector<uint8_t>& buffer, const std::string& fileName);

    static std::vector<std::string> getSupportedExtensions();

    static bool isSupportedExtension(const std::string& extension);

    void setMaxRecursionDepth(int32_t maxDepth);

    int32_t getMaxRecursionDepth() const;

    void setMemoryLimit(size_t limitMB);

    size_t getMemoryLimit() const;

    void setReferenceResolutionEnabled(bool enable);

    bool isReferenceResolutionEnabled() const;

private:
    std::string m_hpcEndpoint;
    int m_numThreads = 0;

    mutable std::shared_mutex processingMutex;
    std::atomic<bool> processingInProgress{false};
    std::atomic<bool> shutdownRequested{false};

    std::atomic<int32_t> maxRecursionDepth{safety::MAX_RECURSION_DEPTH};
    std::atomic<size_t> memoryLimitMB{static_cast<size_t>(std::numeric_limits<int64_t>::max() / (1024 * 1024))}; // Essentially unlimited (4.6EB)
    std::atomic<bool> referenceResolutionEnabled{true};

    ProcessingStats stats;

    bool ProcessPrim(void* prim,
                    std::vector<MeshData>& meshDataArray,
                    const glm::mat4& parentTransform,
                    int32_t depth);

    bool ExtractMeshData(void* mesh,
                        MeshData& outMeshData,
                        const glm::mat4& worldTransform);

    glm::mat4 GetLocalTransform(void* prim);

    void ExtractReferencePaths(const void* stage,
                              std::vector<std::string>& outReferencePaths);

    std::vector<std::string> ExtractClipsFromRawContent(const std::vector<uint8_t>& buffer);

    void ExtractReferencePathsFromPrim(const void* prim,
                                      std::vector<std::string>& outReferencePaths);

    void ListPrimHierarchy(const void* prim, int depth);

    bool validateTransform(const glm::mat4& transform) const;

    bool validateFilePath(const std::string& filePath, bool checkExistence = true) const;

    std::vector<uint8_t> preprocessUsdContent(const std::vector<uint8_t>& buffer);

    bool checkMemoryLimit(size_t additionalBytes = 0) const;

    void normalizeUVCoordinates(std::vector<glm::vec2>& uvs);

    void normalizeUVCoordinatesParallel(std::vector<glm::vec2>& uvs);

    bool validateMeshIndices(std::vector<uint32_t>& indices, size_t vertexCount);

    bool calculateMeshNormals(const std::vector<glm::vec3>& points,
                             const std::vector<uint32_t>& indices,
                             std::vector<glm::vec3>& outNormals);

    void extractUVCoordinates(void* mesh, MeshData& meshData);

    bool hasEmptyGeometry(const std::vector<MeshData>& meshData) const;

    bool resolveReferences(const void* stage,
                          const std::vector<uint8_t>& buffer,
                          const std::string& fileName,
                          std::vector<MeshData>& outMeshData,
                          ProgressCallback progressCallback);

    bool loadReferencedFile(const std::string& filePath,
                           std::vector<MeshData>& outMeshData,
                           ProgressCallback progressCallback);
};