// Copyright 2020 The Khronos Group

#pragma once

#include "anari/backend/DeviceImpl.h"
#include "anari/backend/LibraryImpl.h"
#include "UsdBaseObject.h"

#include <vector>
#include <memory>
#include <limits>

#ifdef ANARI_USD_ENABLE_MPI
#include <thread>
#include <atomic>
#include "UsdBridgeZmqBroker.h"
#endif

#ifdef _WIN32
#ifdef anari_library_usd_EXPORTS
#define USDDevice_INTERFACE __declspec(dllexport)
#else
#define USDDevice_INTERFACE __declspec(dllimport)
#endif
#endif

extern "C"
{
#ifdef _WIN32
  USDDevice_INTERFACE void __cdecl anari_library_usd_init();
#else
  void anari_library_usd_init();
#endif
}

class UsdDevice;
class UsdDeviceInternals;
class UsdBaseObject;
class UsdVolume;

struct UsdDeviceData
{
  UsdSharedString* hostName = nullptr;
  UsdSharedString* outputPath = nullptr;
  bool createNewSession = false;
  bool outputBinary = true;
  bool writeAtCommit = false;
  bool autoFlushOnGeometryCommit = false; // Flush USD + ZMQ on geometry commit (OFF, renderFrame-only)

  double timeStep = 0.0;

  bool outputMaterial = true;
  bool outputPreviewSurfaceShader = false;
  bool outputMdlShader = false;
  bool useDisplayColorOpacity = false;
  bool selectiveFileSaving = false; // Only save geometry and texture files
};

class UsdDevice : public anari::DeviceImpl, public UsdParameterizedBaseObject<UsdDevice, UsdDeviceData>
{
  public:

    UsdDevice();
    UsdDevice(ANARILibrary library);
    ~UsdDevice();


    ANARIArray1D newArray1D(const void *appMemory,
      ANARIMemoryDeleter deleter,
      const void *userdata,
      ANARIDataType,
      uint64_t numItems1) override;

    ANARIArray2D newArray2D(const void *appMemory,
      ANARIMemoryDeleter deleter,
      const void *userdata,
      ANARIDataType,
      uint64_t numItems1,
      uint64_t numItems2) override;

    ANARIArray3D newArray3D(const void *appMemory,
      ANARIMemoryDeleter deleter,
      const void *userdata,
      ANARIDataType,
      uint64_t numItems1,
      uint64_t numItems2,
      uint64_t numItems3) override;

    void* mapArray(ANARIArray) override;
    void unmapArray(ANARIArray) override;


    ANARILight newLight(const char *type) override;

    ANARICamera newCamera(const char *type) override;

    ANARIGeometry newGeometry(const char *type);
    ANARISpatialField newSpatialField(const char *type) override;

    ANARISurface newSurface() override;
    ANARIVolume newVolume(const char *type) override;


    ANARIMaterial newMaterial(const char *material_type) override;

    ANARISampler newSampler(const char *type) override;


    ANARIGroup newGroup() override;

    ANARIInstance newInstance(const char *type) override;


    ANARIWorld newWorld() override;


    ANARIObject newObject(const char *objectType, const char *type) override;

    void (*getProcAddress(const char *name))(void) override;


    const char ** getObjectSubtypes(ANARIDataType objectType) override;
    const void* getObjectInfo(ANARIDataType objectType,
        const char* objectSubtype,
        const char* infoName,
        ANARIDataType infoType) override;
    const void* getParameterInfo(ANARIDataType objectType,
        const char* objectSubtype,
        const char* parameterName,
        ANARIDataType parameterType,
        const char* infoName,
        ANARIDataType infoType) override;


    void setParameter(ANARIObject object,
      const char *name,
      ANARIDataType type,
      const void *mem) override;

    void unsetParameter(ANARIObject object, const char *name) override;
    void unsetAllParameters(ANARIObject o) override;

    void* mapParameterArray1D(ANARIObject o,
        const char* name,
        ANARIDataType dataType,
        uint64_t numElements1,
        uint64_t *elementStride) override;
    void* mapParameterArray2D(ANARIObject o,
        const char* name,
        ANARIDataType dataType,
        uint64_t numElements1,
        uint64_t numElements2,
        uint64_t *elementStride) override;
    void* mapParameterArray3D(ANARIObject o,
        const char* name,
        ANARIDataType dataType,
        uint64_t numElements1,
        uint64_t numElements2,
        uint64_t numElements3,
        uint64_t *elementStride) override;
    void unmapParameterArray(ANARIObject o,
        const char* name) override;

    void commitParameters(ANARIObject object) override;

    void release(ANARIObject _obj) override;
    void retain(ANARIObject _obj) override;


    int getProperty(ANARIObject object,
      const char *name,
      ANARIDataType type,
      void *mem,
      uint64_t size,
      uint32_t mask) override;


    ANARIFrame newFrame() override;

    const void *frameBufferMap(ANARIFrame fb,
        const char *channel,
        uint32_t *width,
        uint32_t *height,
        ANARIDataType *pixelType) override;

    void frameBufferUnmap(ANARIFrame fb, const char *channel) override;


    ANARIRenderer newRenderer(const char *type) override;

    void renderFrame(ANARIFrame frame) override;
    int frameReady(ANARIFrame frame, ANARIWaitMask mask) override;
    void discardFrame(ANARIFrame) override {}


    void filterSetParam(
      const char *name,
      ANARIDataType type,
      const void *mem,
      UsdDevice* device) override;

    void filterResetParam(
      const char *name) override;

    void commit(UsdDevice* device) override;

    void remove(UsdDevice* device) override {}


    bool isInitialized() { return getUsdBridge() != nullptr; }
    UsdBridge* getUsdBridge();

    bool nameExists(const char* name);

    void addToCommitList(UsdBaseObject* object, bool commitData);
    bool isFlushingCommitList() const { return lockCommitList; }

    void addToVolumeList(UsdVolume* volume);
    void removeFromVolumeList(UsdVolume* volume);

    void addToResourceStringList(UsdSharedString* sharedString);

    // Geometry commits can happen before renderFrame. Record the effective
    // ANARI usd::time for the current frame so notifications can carry the
    // simulation time instead of the (usually zero) device-level usd::time.
    void RecordFrameGeometryTime(double t);
    double ConsumeFrameGeometryTime();

#ifdef CHECK_MEMLEAKS
    void logObjAllocation(const UsdBaseObject* ptr);
    void logObjDeallocation(const UsdBaseObject* ptr);
    std::vector<const UsdBaseObject*> allocatedObjects;

    void logStrAllocation(const UsdSharedString* ptr);
    void logStrDeallocation(const UsdSharedString* ptr);
    std::vector<const UsdSharedString*> allocatedStrings;

    void logRawAllocation(const void* ptr);
    void logRawDeallocation(const void* ptr);
    std::vector<const void*> allocatedRawMemory;

    bool isObjAllocated(const UsdBaseObject* ptr) const;
    bool isStrAllocated(const UsdSharedString* ptr) const;
    bool isRawAllocated(const void* ptr) const;
#endif

    void reportStatus(void* source,
      ANARIDataType sourceType,
      ANARIStatusSeverity severity,
      ANARIStatusCode statusCode,
      const char *format, ...);
    void reportStatus(void* source,
      ANARIDataType sourceType,
      ANARIStatusSeverity severity,
      ANARIStatusCode statusCode,
      const char *format,
      va_list* arglist);

  protected:
    UsdBaseObject* getBaseObjectPtr(ANARIObject object);


    bool deferCommit(UsdDevice* device) { return false; };
    bool doCommitData(UsdDevice* device) { return false; };
    void doCommitRefs(UsdDevice* device) {};


    void clearCommitList();
    void flushCommitList();
    void clearDeviceParameters();
    void clearResourceStringList();

    void initializeBridge();

    const char* makeUniqueName(const char* name);

    ANARIArray CreateDataArray(const void *appMemory,
      ANARIMemoryDeleter deleter,
      const void *userData,
      ANARIDataType dataType,
      uint64_t numItems1,
      int64_t byteStride1,
      uint64_t numItems2,
      int64_t byteStride2,
      uint64_t numItems3,
      int64_t byteStride3);

    template<int typeInt>
    void writeTypeToUsd();

    void removePrimsFromUsd(bool onlyRemoveHandles = false);

    std::unique_ptr<UsdDeviceInternals> internals;

    bool bridgeInitAttempt = false;

    using CommitListType = std::pair<helium::IntrusivePtr<UsdBaseObject>, bool>;
    std::vector<CommitListType> commitList;
    std::vector<UsdBaseObject*> removeList;
    std::vector<UsdVolume*> volumeList;
    bool lockCommitList = false;

    std::vector<helium::IntrusivePtr<UsdSharedString>> resourceStringList;

    ANARIStatusCallback statusFunc = nullptr;
    const void* statusUserData = nullptr;
    ANARIStatusCallback userSetStatusFunc = nullptr;
    const void* userSetStatusUserData = nullptr;

    bool disableGarbageCollect_ = false;

   private:
    bool autoFlushOnGeometryCommit = false;
    bool hasPendingFlush_ = false; // reserved for future use
    std::chrono::steady_clock::time_point lastFlushTime_;
    // Last ANARI usd::time value. Notifications carry it (seconds * 1000) so
    // clients can label samples with the same time ParaView displays instead
    // of a wall-clock or synthetic counter.
    double lastTimeStep_ = 0.0;
    // Highest geometry usd::time seen since the previous renderFrame.
    double frameGeometryTime_ = std::numeric_limits<double>::quiet_NaN();
    void FlushSceneAndNotify();

  #ifdef ANARI_USD_ENABLE_MPI
    int mpiRank = 0;
    int mpiSize = 1;
    bool mpiAvailable = false;
    bool mpiInitializedByUs_{false};

    std::unique_ptr<usd_bridge::ZmqBroker> zmqBroker_;
    std::unique_ptr<usd_bridge::ZmqWorker> zmqWorker_;

    void ServeFileRequests();

    std::thread fileServingThread_;
    std::atomic<bool> fileServingActive_{false};
    void FileServingThreadLoop();

    uint32_t frameCounter_{0};
    void NotifyFrameReady(double timestep);
  #endif
};

