// Copyright 2020 The Khronos Group
// SPDX-License-Identifier: Apache-2.0

#include "UsdDevice.h"
#include "UsdBridgedBaseObject.h"
#include "UsdDataArray.h"
#include "UsdGeometry.h"
#include "UsdSpatialField.h"
#include "UsdSurface.h"
#include "UsdVolume.h"
#include "UsdInstance.h"
#include "UsdGroup.h"
#include "UsdMaterial.h"
#include "UsdSampler.h"
#include "UsdWorld.h"
#include "UsdRenderer.h"
#include "UsdFrame.h"
#include "UsdLight.h"
#include "UsdCamera.h"
#include "UsdDeviceQueries.h"
#include "UsdBridge/UsdBridgeMemoryStore.h"

#include <cstdarg>
#include <cstdio>
#include <set>
#include <memory>
#include <sstream>
#include <algorithm>
#include <limits>
#include <filesystem>
#include <thread>
#include <atomic>
#include <system_error>
#include <thread>
#include <chrono>


static char deviceName[] = "usd";

class UsdDeviceInternals
{
public:
  UsdDeviceInternals()
  {
  }

  bool CreateNewBridge(const UsdDeviceData& deviceParams, UsdBridgeLogCallback bridgeStatusFunc, void* userData)
  {
    if (bridge.get())
      bridge->CloseSession();

    UsdBridgeSettings bridgeSettings = {
      UsdSharedString::c_str(deviceParams.hostName),
      outputLocation.c_str(),
      deviceParams.createNewSession,
      deviceParams.outputBinary,
      deviceParams.outputPreviewSurfaceShader,
      deviceParams.outputMdlShader
    };

    bridge = std::make_unique<UsdBridge>(bridgeSettings);

    bridge->SetExternalSceneStage(externalSceneStage);
    bridge->SetEnableSaving(this->enableSaving);
    bridge->SetSelectiveFileSaving(deviceParams.selectiveFileSaving);

    bridgeStatusFunc(UsdBridgeLogLevel::STATUS, userData, "Initializing UsdBridge Session");

    bool createSuccess = bridge->OpenSession(bridgeStatusFunc, userData);

    if (!createSuccess)
    {
      bridge = nullptr;
      bridgeStatusFunc(UsdBridgeLogLevel::STATUS, userData, "UsdBridge Session initialization failed.");
    }
    else
    {
      bridgeStatusFunc(UsdBridgeLogLevel::STATUS, userData, "UsdBridge Session initialization successful.");
    }

    return createSuccess;
  }

  std::string outputLocation;
  bool enableSaving = false; // Default to memory-only mode
  std::unique_ptr<UsdBridge> bridge;
  SceneStagePtr externalSceneStage{nullptr};

  std::set<std::string> uniqueNames;
};

//---- Make sure to update clearDeviceParameters() on refcounted objects
DEFINE_PARAMETER_MAP(UsdDevice,
  REGISTER_PARAMETER_MACRO("usd::serialize.hostName", ANARI_STRING, hostName)
  REGISTER_PARAMETER_MACRO("usd::serialize.location", ANARI_STRING, outputPath)
  REGISTER_PARAMETER_MACRO("usd::serialize.newSession", ANARI_BOOL, createNewSession)
  REGISTER_PARAMETER_MACRO("usd::serialize.outputBinary", ANARI_BOOL, outputBinary)
  REGISTER_PARAMETER_MACRO("usd::time", ANARI_FLOAT64, timeStep)
  REGISTER_PARAMETER_MACRO("usd::writeAtCommit", ANARI_BOOL, writeAtCommit)
  REGISTER_PARAMETER_MACRO("usd::output.material", ANARI_BOOL, outputMaterial)
  REGISTER_PARAMETER_MACRO("usd::output.previewSurfaceShader", ANARI_BOOL, outputPreviewSurfaceShader)
  REGISTER_PARAMETER_MACRO("usd::output.mdlShader", ANARI_BOOL, outputMdlShader)
  REGISTER_PARAMETER_MACRO("usd::selectiveFileSaving", ANARI_BOOL, selectiveFileSaving)
)

void UsdDevice::clearDeviceParameters()
{
  filterResetParam("usd::serialize.hostName");
  filterResetParam("usd::serialize.location");
  transferWriteToReadParams();
}
//----

UsdDevice::UsdDevice()
  : UsdParameterizedBaseObject(ANARI_DEVICE)
  , internals(std::make_unique<UsdDeviceInternals>())
{
}

UsdDevice::UsdDevice(ANARILibrary library)
  : DeviceImpl(library)
  , UsdParameterizedBaseObject(ANARI_DEVICE)
  , internals(std::make_unique<UsdDeviceInternals>())
{}

UsdDevice::~UsdDevice()
{
  // Stop file serving thread before cleanup
  #ifdef ANARI_USD_ENABLE_MPI
  if (fileServingActive_) {
    fileServingActive_ = false;
    if (fileServingThread_.joinable()) {
      fileServingThread_.join();
    }
  }
  #endif

  // Make sure no more references are held before cleaning up the device (and checking for memleaks)
  clearCommitList(); 

  clearDeviceParameters(); // Release device parameters with object references

  clearResourceStringList(); // Do the same for resource string references

  //internals->bridge->SaveScene(); //Uncomment to test cleanup of usd files.
  
  // Cleanup memory store
  CleanupMemoryStore();

#ifdef CHECK_MEMLEAKS
  if(!allocatedObjects.empty() || !allocatedStrings.empty() || !allocatedRawMemory.empty())
  {
    std::stringstream errstream;
    errstream << "USD Device memleak reported for: ";
    for(auto ptr : allocatedObjects)
      errstream << "Object ptr: 0x" << std::hex << ptr << " of type: " << std::dec << ptr->getType() << "; ";
    for(auto ptr : allocatedStrings)
      errstream << "String ptr: 0x" << std::hex << ptr << " with content: " << std::dec << ptr->c_str() << "; ";
    for(auto ptr : allocatedRawMemory)
      errstream << "Raw ptr: 0x" << std::hex << ptr << std::dec << "; ";

    reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_ERROR, ANARI_STATUS_INVALID_OPERATION, errstream.str().c_str());
  }
  else
  {
    reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO, ANARI_STATUS_NO_ERROR, "Reference memleak check complete, no issues found.");
  }
  //assert(allocatedObjects.empty());
#endif
}

void UsdDevice::reportStatus(void* source,
  ANARIDataType sourceType,
  ANARIStatusSeverity severity,
  ANARIStatusCode statusCode,
  const char *format, ...)
{
  va_list arglist;

  va_start(arglist, format);
  reportStatus(source, sourceType, severity, statusCode, format, arglist);
  va_end(arglist);
}

static void reportBridgeStatus(UsdBridgeLogLevel level, void* device, const char *message)
{
  ANARIStatusSeverity severity = UsdBridgeLogLevelToAnariSeverity(level);

  va_list arglist;
  ((UsdDevice*)device)->reportStatus(nullptr, ANARI_UNKNOWN, severity, ANARI_STATUS_NO_ERROR, message, arglist);
}

void UsdDevice::reportStatus(void* source,
  ANARIDataType sourceType,
  ANARIStatusSeverity severity,
  ANARIStatusCode statusCode,
  const char *format,
  va_list& arglist)
{
  va_list arglist_copy;
  va_copy(arglist_copy, arglist);
  int count = std::vsnprintf(nullptr, 0, format, arglist);

  lastStatusMessage.resize(count + 1);

  std::vsnprintf(lastStatusMessage.data(), count + 1, format, arglist_copy);
  va_end(arglist_copy);

  if (statusFunc != nullptr)
  {
    statusFunc(
      statusUserData,
      (ANARIDevice)this,
      (ANARIObject)source,
      sourceType,
      severity,
      statusCode,
      lastStatusMessage.data());
  }
}

void UsdDevice::filterSetParam(
  const char *name,
  ANARIDataType type,
  const void *mem,
  UsdDevice* device)
{
  if (strEquals(name, "usd::garbageCollect"))
  {
    // Perform garbage collection on usd objects (needs to move into the user interface)
    if(internals->bridge)
      internals->bridge->GarbageCollect();
  }
  else if(strEquals(name, "usd::removeUnusedNames"))
  {
    internals->uniqueNames.clear();
  }
  else if (strEquals(name, "usd::connection.logVerbosity")) // 0 <= verbosity <= USDBRIDGE_MAX_LOG_VERBOSITY, with USDBRIDGE_MAX_LOG_VERBOSITY being the loudest
  {
    if(type == ANARI_INT32)
      UsdBridge::SetConnectionLogVerbosity(*(reinterpret_cast<const int*>(mem)));
  }
  else if(strEquals(name, "usd::sceneStage"))
  {
    if(type == ANARI_VOID_POINTER)
      internals->externalSceneStage = const_cast<void *>(mem);
  }
  else if (strEquals(name, "usd::enableSaving"))
  {
    if(type == ANARI_BOOL)
    {
      internals->enableSaving = *(reinterpret_cast<const bool*>(mem));
      if(internals->bridge)
        internals->bridge->SetEnableSaving(internals->enableSaving);
    }
  }
  else if (strEquals(name, "statusCallback") && type == ANARI_STATUS_CALLBACK)
  {
    userSetStatusFunc = (ANARIStatusCallback)mem;
  }
  else if (strEquals(name, "statusCallbackUserData") && type == ANARI_VOID_POINTER)
  {
    userSetStatusUserData = const_cast<void *>(mem);
  }
  else
  {
    setParam(name, type, mem, this);
  }
}

void UsdDevice::filterResetParam(const char * name)
{
  if (strEquals(name, "statusCallback"))
  {
    userSetStatusFunc = nullptr;
  }
  else if (strEquals(name, "statusCallbackUserData"))
  {
    userSetStatusUserData = nullptr;
  }
  else if (!strEquals(name, "usd::garbageCollect")
    && !strEquals(name, "usd::removeUnusedNames"))
  {
    resetParam(name);
  }
}

void UsdDevice::commit(UsdDevice* device)
{
  transferWriteToReadParams();

  if(!bridgeInitAttempt)
  {
    initializeBridge();
  }
  else
  {
    const UsdDeviceData& paramData = getReadParams();
    internals->bridge->UpdateBeginEndTime(paramData.timeStep);
  }
}

void UsdDevice::initializeBridge()
{
  const UsdDeviceData& paramData = getReadParams();

  bridgeInitAttempt = true;

  statusFunc = userSetStatusFunc ? userSetStatusFunc : defaultStatusCallback();
  statusUserData = userSetStatusUserData ? userSetStatusUserData : defaultStatusCallbackUserPtr();

  if(paramData.outputPath)
    internals->outputLocation = paramData.outputPath->c_str();

  if(internals->outputLocation.empty()) {
    auto *envLocation = getenv("ANARI_USD_SERIALIZE_LOCATION");
    if (envLocation) {
      internals->outputLocation = envLocation;
      reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO, ANARI_STATUS_NO_ERROR,
        "Usd Device parameter 'usd::serialize.location' using ANARI_USD_SERIALIZE_LOCATION value");
    }
  }

  if (internals->outputLocation.empty())
  {
    if(internals->enableSaving)
    {
      reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_WARNING, ANARI_STATUS_INVALID_ARGUMENT,
        "Usd Device parameter 'usd::serialize.location' not set, defaulting to './'");
      internals->outputLocation = "./";
    }
    else
    {
      // For memory-only mode, location doesn't matter
      internals->outputLocation = "memory://";
    }
  }

#ifdef ANARI_USD_ENABLE_MPI
  // Read MPI rank from environment variables
  const char* slurmprocid = getenv("SLURM_PROCID");
  const char* slurmntasks = getenv("SLURM_NTASKS");
  const char* ompirank = getenv("OMPI_COMM_WORLD_RANK");
  const char* ompisize = getenv("OMPI_COMM_WORLD_SIZE");
  const char* pmirank = getenv("PMI_RANK");
  const char* pmisize = getenv("PMI_SIZE");

  if (slurmprocid && slurmntasks) {
    mpiRank = std::atoi(slurmprocid);
    mpiSize = std::atoi(slurmntasks);
    mpiAvailable = true;
  }
  else if (ompirank && ompisize) {
    mpiRank = std::atoi(ompirank);
    mpiSize = std::atoi(ompisize);
    mpiAvailable = true;
  }
  else if (pmirank && pmisize) {
    mpiRank = std::atoi(pmirank);
    mpiSize = std::atoi(pmisize);
    mpiAvailable = true;
  }

  if (mpiAvailable && mpiSize > 1) {
    if (mpiRank == 0)
    {
      // Initialize broker on rank 0
      // Wait for ranks 1-N to connect (rank 0 does NOT connect as a worker)
      zmqBroker_ = std::make_unique<usd_bridge::ZmqBroker>(5555);
      if (!zmqBroker_->Initialize(mpiSize - 1))  // Wait for ranks 1-15 only
      {
        reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_ERROR, ANARI_STATUS_UNKNOWN_ERROR,
            "Failed to initialize ZMQ broker on rank 0");
        return;
      }

      // Print connected workers
      const auto& workers = zmqBroker_->GetConnectedWorkers();
      for (const auto& worker : workers)
      {
        reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO, ANARI_STATUS_NO_ERROR,
            "Worker rank %d connected from %s (%s)",
            worker.rank, worker.hostname.c_str(), worker.ib_address.c_str());
      }

      reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO, ANARI_STATUS_NO_ERROR,
          "Rank 0: Broker started. Rank 0 renders AND serves files directly from memory store.");
    }

    else {
      // Initialize worker on rank 1-N
      zmqWorker_ = std::make_unique<usd_bridge::ZmqWorker>("", mpiRank);
      if (!zmqWorker_->Connect()) {
        reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_ERROR,
                     ANARI_STATUS_UNKNOWN_ERROR,
                     "Failed to connect to broker on rank %d", mpiRank);
        return;
      }
      
      // Start background file serving thread
      fileServingActive_ = true;
      fileServingThread_ = std::thread(&UsdDevice::FileServingThreadLoop, this);
      reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO,
                   ANARI_STATUS_NO_ERROR,
                   "Rank %d: Started background file serving thread", mpiRank);
    }
    
    // Initialize memory store for this rank (for memory-only mode)
    InitializeMemoryStore(mpiRank);
    reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO,
                 ANARI_STATUS_NO_ERROR,
                 "Memory store initialized for rank %d", mpiRank);
  }
#endif


  // Only create directories if saving is enabled
  if(internals->enableSaving)
  {
    // Add MPI-aware directory creation with barrier
    std::error_code ec;

#ifdef ANARI_USD_ENABLE_MPI
    if (mpiAvailable) {
      // Only rank 0 creates the directories
      if (mpiRank == 0) {
        std::filesystem::create_directories(internals->outputLocation, ec);
        if (ec) {
          std::stringstream ss;
          ss << "Failed to create USD output directory: " << internals->outputLocation
             << " Error: " << ec.message();
          reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_ERROR,
                       ANARI_STATUS_UNKNOWN_ERROR, ss.str().c_str());
          bridgeInitAttempt = true;
          return;
        }
      }

      // Simple barrier: other ranks wait briefly for rank 0 to create directory
      // This is crude but avoids MPI dependencies
      if (mpiRank != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100 * (mpiRank / 4 + 1)));

        // Verify directory exists
        int retries = 10;
        while (retries > 0 && !std::filesystem::exists(internals->outputLocation)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          retries--;
        }

        if (!std::filesystem::exists(internals->outputLocation)) {
          std::stringstream ss;
          ss << "Rank " << mpiRank << ": USD output directory not found: "
             << internals->outputLocation;
          reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_ERROR,
                       ANARI_STATUS_UNKNOWN_ERROR, ss.str().c_str());
          bridgeInitAttempt = true;
          return;
        }
      }
    } else {
      // Non-MPI case: just create directories normally
      std::filesystem::create_directories(internals->outputLocation, ec);
      if (ec) {
        std::stringstream ss;
        ss << "Failed to create USD output directory: " << internals->outputLocation
             << " Error: " << ec.message();
        reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_ERROR,
                     ANARI_STATUS_UNKNOWN_ERROR, ss.str().c_str());
        bridgeInitAttempt = true;
        return;
      }
    }
#else
    // Non-MPI build
    std::filesystem::create_directories(internals->outputLocation, ec);
    if (ec) {
      std::stringstream ss;
      ss << "Failed to create USD output directory: " << internals->outputLocation
           << " Error: " << ec.message() << "\n"
           << "On compute nodes, set ANARI_USD_SERIALIZE_LOCATION to a shared filesystem path.";
      reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_ERROR,
                   ANARI_STATUS_UNKNOWN_ERROR, ss.str().c_str());
      bridgeInitAttempt = true;
      return;
    }
#endif
  } // End of enableSaving check


  if (!internals->CreateNewBridge(paramData, &reportBridgeStatus, this))
  {
    reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_ERROR, ANARI_STATUS_UNKNOWN_ERROR,
      "Usd Bridge failed to load");
    bridgeInitAttempt = true;
    return;
  }
}


ANARIArray UsdDevice::CreateDataArray(const void *appMemory,
  ANARIMemoryDeleter deleter,
  const void *userData,
  ANARIDataType dataType,
  uint64_t numItems1,
  int64_t byteStride1,
  uint64_t numItems2,
  int64_t byteStride2,
  uint64_t numItems3,
  int64_t byteStride3)
{
  if (!appMemory)
  {
    UsdDataArray* object = new UsdDataArray(dataType, numItems1, numItems2, numItems3, this);
#ifdef CHECK_MEMLEAKS
    logObjAllocation(object);
#endif

    return (ANARIArray)(object);
  }
  else
  {
    UsdDataArray* object = new UsdDataArray(appMemory, deleter, userData,
      dataType, numItems1, byteStride1, numItems2, byteStride2, numItems3, byteStride3,
      this);
#ifdef CHECK_MEMLEAKS
    logObjAllocation(object);
#endif

    return (ANARIArray)(object);
  }
}

ANARIArray1D UsdDevice::newArray1D(const void *appMemory,
  ANARIMemoryDeleter deleter,
  const void *userData,
  ANARIDataType type,
  uint64_t numItems)
{
  return (ANARIArray1D)CreateDataArray(appMemory, deleter, userData,
    type, numItems, 0, 1, 0, 1, 0);
}

ANARIArray2D UsdDevice::newArray2D(const void *appMemory,
  ANARIMemoryDeleter deleter,
  const void *userData,
  ANARIDataType type,
  uint64_t numItems1,
  uint64_t numItems2)
{
  return (ANARIArray2D)CreateDataArray(appMemory, deleter, userData,
    type, numItems1, 0, numItems2, 0, 1, 0);
}

ANARIArray3D UsdDevice::newArray3D(const void *appMemory,
  ANARIMemoryDeleter deleter,
  const void *userData,
  ANARIDataType type,
  uint64_t numItems1,
  uint64_t numItems2,
  uint64_t numItems3)
{
  return (ANARIArray3D)CreateDataArray(appMemory, deleter, userData,
    type, numItems1, 0, numItems2, 0, numItems3, 0);
}

void * UsdDevice::mapArray(ANARIArray array)
{
  return array ? AnariToUsdObjectPtr(array)->map(this) : nullptr;
}

void UsdDevice::unmapArray(ANARIArray array)
{
  if(array)
    AnariToUsdObjectPtr(array)->unmap(this);
}

ANARISampler UsdDevice::newSampler(const char *type)
{
  const char* name = makeUniqueName("Sampler");
  UsdSampler* object = new UsdSampler(name, type, this);
#ifdef CHECK_MEMLEAKS
  logObjAllocation(object);
#endif

  return (ANARISampler)(object);
}

ANARIMaterial UsdDevice::newMaterial(const char *material_type)
{
  const char* name = makeUniqueName("Material");
  UsdMaterial* object = new UsdMaterial(name, material_type, this);
#ifdef CHECK_MEMLEAKS
  logObjAllocation(object);
#endif

  return (ANARIMaterial)(object);
}

ANARIGeometry UsdDevice::newGeometry(const char *type)
{
  const char* name = makeUniqueName("Geometry");
  UsdGeometry* object = new UsdGeometry(name, type, this);
#ifdef CHECK_MEMLEAKS
  logObjAllocation(object);
#endif

  return (ANARIGeometry)(object);
}

ANARISpatialField UsdDevice::newSpatialField(const char * type)
{
  const char* name = makeUniqueName("SpatialField");
  UsdSpatialField* object = new UsdSpatialField(name, type, this);
#ifdef CHECK_MEMLEAKS
  logObjAllocation(object);
#endif

  return (ANARISpatialField)(object);
}

ANARISurface UsdDevice::newSurface()
{
  const char* name = makeUniqueName("Surface");
  UsdSurface* object = new UsdSurface(name, this);
#ifdef CHECK_MEMLEAKS
  logObjAllocation(object);
#endif

  return (ANARISurface)(object);
}

ANARIVolume UsdDevice::newVolume(const char *type)
{
  const char* name = makeUniqueName("Volume");
  UsdVolume* object = new UsdVolume(name, this);
#ifdef CHECK_MEMLEAKS
  logObjAllocation(object);
#endif

  return (ANARIVolume)(object);
}

ANARIGroup UsdDevice::newGroup()
{
  const char* name = makeUniqueName("Group");
  UsdGroup* object = new UsdGroup(name, this);
#ifdef CHECK_MEMLEAKS
  logObjAllocation(object);
#endif

  return (ANARIGroup)(object);
}

ANARIInstance UsdDevice::newInstance(const char */*type*/)
{
  const char* name = makeUniqueName("Instance");
  UsdInstance* object = new UsdInstance(name, this);
#ifdef CHECK_MEMLEAKS
  logObjAllocation(object);
#endif

  return (ANARIInstance)(object);
}

ANARIWorld UsdDevice::newWorld()
{
  const char* name = makeUniqueName("World");
  UsdWorld* object = new UsdWorld(name, this);
#ifdef CHECK_MEMLEAKS
  logObjAllocation(object);
#endif

  return (ANARIWorld)(object);
}

ANARILight UsdDevice::newLight(const char *type)
{
  const char* name = makeUniqueName("Light");
  UsdLight* object = new UsdLight(name, type, this);
#ifdef CHECK_MEMLEAKS
  logObjAllocation(object);
#endif

  return (ANARILight)(object);
}

ANARICamera UsdDevice::newCamera(const char *type)
{
  const char* name = makeUniqueName("Camera");
  UsdCamera* object = new UsdCamera(name, type, this);
#ifdef CHECK_MEMLEAKS
  logObjAllocation(object);
#endif

  return (ANARICamera)(object);
}

const char **UsdDevice::getObjectSubtypes(ANARIDataType objectType)
{
  return anari::usd::query_object_types(objectType);
}

const void *UsdDevice::getObjectInfo(ANARIDataType objectType,
    const char *objectSubtype,
    const char *infoName,
    ANARIDataType infoType)
{
  return anari::usd::query_object_info(
      objectType, objectSubtype, infoName, infoType);
}

const void *UsdDevice::getParameterInfo(ANARIDataType objectType,
    const char *objectSubtype,
    const char *parameterName,
    ANARIDataType parameterType,
    const char *infoName,
    ANARIDataType infoType)
{
  return anari::usd::query_param_info(objectType,
      objectSubtype,
      parameterName,
      parameterType,
      infoName,
      infoType);
}


ANARIRenderer UsdDevice::newRenderer(const char *type)
{
  UsdRenderer* object = new UsdRenderer();
#ifdef CHECK_MEMLEAKS
  logObjAllocation(object);
#endif

  return (ANARIRenderer)(object);
}

UsdBridge* UsdDevice::getUsdBridge()
{
  return internals->bridge.get();
}

void UsdDevice::renderFrame(ANARIFrame frame)
{
  // Always commit device changes if not initialized, otherwise no conversion can be performed.
  if(!bridgeInitAttempt)
    initializeBridge();

  if(!isInitialized())
      return;

  flushCommitList();

  internals->bridge->ResetResourceUpdateState(); // Reset the modified flags for committed shared resources

  if(frame)
    AnariToUsdObjectPtr(frame)->saveUsd(this);

#ifdef ANARI_USD_ENABLE_MPI
  // Send commit notification to laptop client
  if (zmqWorker_ && zmqWorker_->IsConnected() && frame) {
    // Get the frame filename and send notification
    const char* frameFilename = "scene.usda"; // Default filename
    auto* fileEntry = g_rankMemoryStore ? g_rankMemoryStore->GetFile(frameFilename) : nullptr;
    uint64_t fileSize = fileEntry ? fileEntry->size() : 0;
    uint64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    zmqWorker_->SendCommitNotification(frameFilename, fileSize, timestamp);
  }

  // Serve file requests from laptop client
  if (zmqWorker_ && zmqWorker_->IsConnected()) {
    ServeFileRequests();
  }
#endif
}

#ifdef ANARI_USD_ENABLE_MPI
void UsdDevice::ServeFileRequests()
{
  using namespace usd_bridge;

  // DEBUG: Print on first call to show ServeFileRequests is active
  static bool first_call = true;
  if (first_call) {
    first_call = false;
    reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO,
                 ANARI_STATUS_NO_ERROR,
                 "[DEBUG] Rank %d: ServeFileRequests() is ACTIVE", mpiRank);

    // DEBUG: List all files in memory store
    if (g_rankMemoryStore) {
      auto files = g_rankMemoryStore->ListFiles();
      reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO,
                   ANARI_STATUS_NO_ERROR,
                   "[DEBUG] Rank %d: MemoryStore contains %zu files:", mpiRank, files.size());
      for (const auto& filename : files) {
        const auto* entry = g_rankMemoryStore->GetFile(filename);
        reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO,
                     ANARI_STATUS_NO_ERROR,
                     "[DEBUG]   - %s (%zu bytes, %s)",
                     filename.c_str(), entry->size(), entry->mime_type.c_str());
      }
    }
  }

  ZmqFileRequest request;

  // Non-blocking check for file requests (process multiple requests if available)
  while (zmqWorker_->CheckForFileRequest(request, false)) {
    // DEBUG: Print when request arrives
    reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO,
                 ANARI_STATUS_NO_ERROR,
                 "[DEBUG] Rank %d: Received request type %u for '%s' (req_id=%u)",
                 mpiRank, request.message_type, request.filename, request.request_id);

    // Validate request
    if (request.magic != USD_FILE_MAGIC) {
      reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_WARNING,
                   ANARI_STATUS_INVALID_OPERATION,
                   "Rank %d: Received invalid file request (bad magic)", mpiRank);
      continue;
    }

    // Handle REQ_LIST_FILES - send list of all files in memory
    if (request.message_type == static_cast<uint32_t>(usd_bridge::ZmqMessageType::REQ_LIST_FILES)) {
      reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO,
                   ANARI_STATUS_NO_ERROR,
                   "[DEBUG] Rank %d: Handling REQ_LIST_FILES request", mpiRank);

      auto files = g_rankMemoryStore->ListFiles();

      // Build file list response as JSON string
      std::stringstream jsonResponse;
      jsonResponse << "{\"rank\":" << mpiRank << ",\"files\":[";
      bool first = true;
      for (const auto& filename : files) {
        // Filter out .usda.usda files (duplicate extensions)
        if (filename.find(".usda.usda") != std::string::npos) {
          reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_WARNING,
                       ANARI_STATUS_NO_ERROR,
                       "[DEBUG] Rank %d: Skipping duplicate extension file: %s",
                       mpiRank, filename.c_str());
          continue;
        }
        
        const auto* entry = g_rankMemoryStore->GetFile(filename);
        if (!first) jsonResponse << ",";
        jsonResponse << "{\"name\":\"" << filename << "\","
                     << "\"size\":" << entry->size() << ","
                     << "\"mime\":\"" << entry->mime_type << "\"}";
        first = false;
      }
      jsonResponse << "]}";

      std::string jsonStr = jsonResponse.str();

      // Send as a "file" with special name "__file_list__"
      bool chunkSent = zmqWorker_->SendFileChunk(
            request.request_id,
            "__file_list__.json",
            jsonStr.data(),
            jsonStr.size(),
            jsonStr.size(),
            0);

      reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO,
                   ANARI_STATUS_NO_ERROR,
                   "[DEBUG] Rank %d: SendFileChunk returned %s",
                   mpiRank, chunkSent ? "TRUE" : "FALSE");

      if (!chunkSent) {
        reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_ERROR,
                     ANARI_STATUS_UNKNOWN_ERROR,
                     "Rank %d: Failed to send file list chunk", mpiRank);
      }

      // ALWAYS send completion (even if chunk failed, for protocol consistency)
      bool completeSent = zmqWorker_->SendFileComplete(request.request_id, "__file_list__.json", jsonStr.size());

      reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO,
                   ANARI_STATUS_NO_ERROR,
                   "[DEBUG] Rank %d: SendFileComplete returned %s",
                   mpiRank, completeSent ? "TRUE" : "FALSE");

      reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO,
                   ANARI_STATUS_NO_ERROR,
                   "Rank %d: Sent file list (%zu files) - chunk:%s complete:%s",
                   mpiRank, files.size(),
                   chunkSent ? "OK" : "FAIL",
                   completeSent ? "OK" : "FAIL");
      continue;
    }

    // Handle REQ_GET_FILE - send specific file

    // Look up file in memory store
    const auto* fileEntry = g_rankMemoryStore->GetFile(request.filename);

    if (!fileEntry) {
      // DEBUG: Print what files ARE available
      auto files = g_rankMemoryStore->ListFiles();
      reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_WARNING,
                   ANARI_STATUS_NO_ERROR,
                   "[DEBUG] Rank %d: File '%s' NOT FOUND. Available files:",
                   mpiRank, request.filename);
      for (const auto& filename : files) {
        reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO,
                     ANARI_STATUS_NO_ERROR,
                     "[DEBUG]   - %s", filename.c_str());
      }

      // File not found - send error response
      zmqWorker_->SendNoFile(request.request_id, request.filename);
      reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO,
                   ANARI_STATUS_NO_ERROR,
                   "Rank %d: File not found: %s", mpiRank, request.filename);
      continue;
    }

    // Determine chunk size (use requested or default)
    size_t chunkSize = request.chunk_size > 0 ? request.chunk_size : DEFAULT_CHUNK_SIZE;

    // Send file in chunks
    size_t offset = 0;
    size_t totalSize = fileEntry->data.size();

    reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO,
                 ANARI_STATUS_NO_ERROR,
                 "Rank %d: Sending file %s (%zu bytes) in %zu-byte chunks",
                 mpiRank, request.filename, totalSize, chunkSize);

    while (offset < totalSize) {
      size_t sendSize = std::min(chunkSize, totalSize - offset);

      if (!zmqWorker_->SendFileChunk(
            request.request_id,
            request.filename,
            fileEntry->data.data() + offset,
            sendSize,
            totalSize,
            offset)) {
        reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_ERROR,
                     ANARI_STATUS_UNKNOWN_ERROR,
                     "Rank %d: Failed to send file chunk for %s at offset %zu",
                     mpiRank, request.filename, offset);
        break;
      }

      offset += sendSize;
    }

    // Send completion message
    if (offset == totalSize) {
      zmqWorker_->SendFileComplete(request.request_id, request.filename, totalSize);
      reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO,
                   ANARI_STATUS_NO_ERROR,
                   "Rank %d: Successfully sent file %s", mpiRank, request.filename);
    }
  }
}

void UsdDevice::FileServingThreadLoop()
{
  reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO,
               ANARI_STATUS_NO_ERROR,
               "[THREAD] Rank %d: File serving thread started", mpiRank);
  
  while (fileServingActive_) {
    // Continuously serve file requests
    ServeFileRequests();
    
    // Small sleep to avoid burning CPU (10ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  
  reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO,
               ANARI_STATUS_NO_ERROR,
               "[THREAD] Rank %d: File serving thread stopped", mpiRank);
}
#endif

const char* UsdDevice::makeUniqueName(const char* name)
{
  std::string proposedBaseName(name);
  proposedBaseName.append("_");

  int postFix = 0;
  std::string proposedName = proposedBaseName + std::to_string(postFix);

  auto empRes = internals->uniqueNames.emplace(proposedName);
  while (!empRes.second)
  {
    ++postFix;
    proposedName = proposedBaseName + std::to_string(postFix);
    empRes = internals->uniqueNames.emplace(proposedName);
  }

  return empRes.first->c_str();
}

bool UsdDevice::nameExists(const char* name)
{
  return internals->uniqueNames.find(name) != internals->uniqueNames.end();
}

void UsdDevice::addToCommitList(UsdBaseObject* object, bool commitData)
{
  if(!object)
    return;

  if(lockCommitList)
  {
    this->reportStatus(object, object->getType(), ANARI_SEVERITY_FATAL_ERROR, ANARI_STATUS_INVALID_OPERATION,
      "Usd device internal error; addToCommitList called while list is locked");
  }
  else
  {
    auto it = std::find_if(commitList.begin(), commitList.end(),
      [&object](const CommitListType& entry) -> bool { return entry.first.ptr == object; });
    if(it == commitList.end())
      commitList.emplace_back(CommitListType(object, commitData));
  }
}

void UsdDevice::clearCommitList()
{
  removePrimsFromUsd(true); // removeList pointers are taken from commitlist

#ifdef CHECK_MEMLEAKS
  for(auto& commitEntry : commitList)
  {
    logObjDeallocation(commitEntry.first.ptr);
  }
#endif

  commitList.resize(0);
}

void UsdDevice::flushCommitList()
{
  lockCommitList = true;

  writeTypeToUsd<(int)ANARI_SAMPLER>();

  writeTypeToUsd<(int)ANARI_SPATIAL_FIELD>();
  writeTypeToUsd<(int)ANARI_GEOMETRY>();
  writeTypeToUsd<(int)ANARI_LIGHT>();

  writeTypeToUsd<(int)ANARI_MATERIAL>();

  writeTypeToUsd<(int)ANARI_SURFACE>();
  writeTypeToUsd<(int)ANARI_VOLUME>();

  writeTypeToUsd<(int)ANARI_GROUP>();
  writeTypeToUsd<(int)ANARI_INSTANCE>();
  writeTypeToUsd<(int)ANARI_WORLD>();

  writeTypeToUsd<(int)ANARI_CAMERA>();

  removePrimsFromUsd();

  clearCommitList();

  lockCommitList = false;
}

void UsdDevice::addToVolumeList(UsdVolume* volume)
{
  auto it = std::find(volumeList.begin(), volumeList.end(), volume);
  if(it == volumeList.end())
    volumeList.emplace_back(volume);
}

void UsdDevice::addToResourceStringList(UsdSharedString* string)
{
  resourceStringList.push_back(helium::IntrusivePtr<UsdSharedString>(string));
}

void UsdDevice::clearResourceStringList()
{
  resourceStringList.resize(0);
}

void UsdDevice::removeFromVolumeList(UsdVolume* volume)
{
  auto it = std::find(volumeList.begin(), volumeList.end(), volume);
  if(it == volumeList.end())
  {
    *it = volumeList.back();
    volumeList.pop_back();
  }
}

template<int typeInt>
void UsdDevice::writeTypeToUsd()
{
  for(auto objCommitPair : commitList)
  {
    auto object = objCommitPair.first;
    bool commitData = objCommitPair.second;

    if((int)object->getType() == typeInt)
    {
      using ObjectType = typename AnariToUsdBridgedObject<typeInt>::Type;
      ObjectType* typedObj = reinterpret_cast<ObjectType*>(object.ptr);

      if(!object->deferCommit(this))
      {
        bool commitRefs = true;
        if(commitData)
          commitRefs = object->doCommitData(this);
        if(commitRefs)
          object->doCommitRefs(this);
      }
      else
      {
        this->reportStatus(object.ptr, object->getType(), ANARI_SEVERITY_ERROR, ANARI_STATUS_INVALID_OPERATION,
          "User forgot to at least once commit an ANARI child object of parent object '%s'", typedObj->getName());
      }

      if(typedObj->getRemovePrim())
      {
        removeList.push_back(object.ptr); // Just raw pointer, removeList is purged upon commitList clear
      }
    }
  }
}

void UsdDevice::removePrimsFromUsd(bool onlyRemoveHandles)
{
  if(!onlyRemoveHandles)
  {
    for(auto baseObj : removeList)
    {
      baseObj->remove(this);
    }
  }
  removeList.resize(0);
}

int UsdDevice::getProperty(ANARIObject object,
    const char *name,
    ANARIDataType type,
    void *mem,
    uint64_t size,
    uint32_t mask)
{
  if ((void *)object == (void *)this)
  {
    if (strEquals(name, "version") && type == ANARI_INT32)
    {
      writeToVoidP(mem, DEVICE_VERSION_BUILD);
      return 1;
    }
    if (strEquals(name, "version.major") && type == ANARI_INT32)
    {
      writeToVoidP(mem, DEVICE_VERSION_MAJOR);
      return 1;
    }
    if (strEquals(name, "version.minor") && type == ANARI_INT32)
    {
      writeToVoidP(mem, DEVICE_VERSION_MINOR);
      return 1;
    }
    if (strEquals(name, "version.patch") && type == ANARI_INT32)
    {
      writeToVoidP(mem, DEVICE_VERSION_PATCH);
      return 1;
    }
    if (strEquals(name, "version.name") && type == ANARI_STRING)
    {
      snprintf((char*)mem, size, "%s", DEVICE_VERSION_NAME);
      return 1;
    }
    else if (strEquals(name, "version.name.size") && type == ANARI_UINT64)
    {
      if (Assert64bitStringLengthProperty(size, UsdLogInfo(this, this, ANARI_DEVICE, "UsdDevice"), "version.name.size"))
      {
        uint64_t nameLen = strlen(DEVICE_VERSION_NAME)+1;
        memcpy(mem, &nameLen, size);
      }
      return 1;
    }
    else if (strEquals(name, "geometryMaxIndex") && type == ANARI_UINT64)
    {
      uint64_t maxIndex = std::numeric_limits<uint64_t>::max(); // Only restricted to int for UsdGeomMesh: GetFaceVertexIndicesAttr() takes a VtArray<int>
      writeToVoidP(mem, maxIndex);
      return 1;
    }
    else if (strEquals(name, "extension") && type == ANARI_STRING_LIST)
    {
      writeToVoidP(mem, anari::usd::query_extensions());
      return 1;
    }
    else if (strEquals(name, "workerCount") && type == ANARI_INT32)
    {
      int32_t workerCount = 0;
#ifdef ANARI_USD_ENABLE_MPI
      if (mpiAvailable && mpiRank == 0 && zmqBroker_)
      {
        // On rank 0, return the number of connected workers
        workerCount = static_cast<int32_t>(zmqBroker_->GetConnectedWorkers().size());
      }
      else if (mpiAvailable && mpiSize > 1)
      {
        // On worker ranks, return 0 (they are workers, not the broker)
        workerCount = 0;
      }
      else
      {
        // Non-MPI mode
        workerCount = 0;
      }
#else
      // Non-MPI build
      workerCount = 0;
#endif
      writeToVoidP(mem, workerCount);
      return 1;
    }
    else if (strEquals(name, "mpiSize") && type == ANARI_INT32)
    {
      int32_t mpiSizeValue = 1;
#ifdef ANARI_USD_ENABLE_MPI
      if (mpiAvailable)
      {
        mpiSizeValue = mpiSize;
      }
#endif
      writeToVoidP(mem, mpiSizeValue);
      return 1;
    }
    else if (strEquals(name, "mpiRank") && type == ANARI_INT32)
    {
      int32_t mpiRankValue = 0;
#ifdef ANARI_USD_ENABLE_MPI
      if (mpiAvailable)
      {
        mpiRankValue = mpiRank;
      }
#endif
      writeToVoidP(mem, mpiRankValue);
      return 1;
    }
  }
  else if(object)
    return AnariToUsdObjectPtr(object)->getProperty(name, type, mem, size, this);

  return 0;
}

ANARIFrame UsdDevice::newFrame()
{
  UsdFrame* object = new UsdFrame(internals->bridge.get());
#ifdef CHECK_MEMLEAKS
  logObjAllocation(object);
#endif

  return (ANARIFrame)(object);
}

const void * UsdDevice::frameBufferMap(
  ANARIFrame fb,
  const char *channel,
  uint32_t *width,
  uint32_t *height,
  ANARIDataType *pixelType)
{
  if (fb)
    return AnariToUsdObjectPtr(fb)->mapBuffer(channel, width, height, pixelType);
  return nullptr;
}

void UsdDevice::frameBufferUnmap(ANARIFrame fb, const char *channel)
{
  if (fb)
    return AnariToUsdObjectPtr(fb)->unmapBuffer(channel);
}

UsdBaseObject* UsdDevice::getBaseObjectPtr(ANARIObject object)
{
  return handleIsDevice(object) ? this : (UsdBaseObject*)object;
}

void UsdDevice::setParameter(ANARIObject object,
  const char *name,
  ANARIDataType type,
  const void *mem)
{
  if(object)
    getBaseObjectPtr(object)->filterSetParam(name, type, mem, this);
}

void UsdDevice::unsetParameter(ANARIObject object, const char * name)
{
  if(object)
    getBaseObjectPtr(object)->filterResetParam(name);
}

void UsdDevice::unsetAllParameters(ANARIObject object)
{
  if(object)
    getBaseObjectPtr(object)->resetAllParams();
}

void *UsdDevice::mapParameterArray1D(ANARIObject object,
    const char *name,
    ANARIDataType dataType,
    uint64_t numElements1,
    uint64_t *elementStride)
{
  auto array = newArray1D(nullptr, nullptr, nullptr, dataType, numElements1);
  setParameter(object, name, ANARI_ARRAY1D, &array);
  *elementStride = anari::sizeOf(dataType);
  bool paramExists = AnariToUsdObjectPtr(array)->useCount() > 1;
  AnariToUsdObjectPtr(array)->refDec(helium::RefType::PUBLIC);
  return paramExists ? mapArray(array) : nullptr;
}

void *UsdDevice::mapParameterArray2D(ANARIObject object,
    const char *name,
    ANARIDataType dataType,
    uint64_t numElements1,
    uint64_t numElements2,
    uint64_t *elementStride)
{
  auto array = newArray2D(nullptr, nullptr, nullptr, dataType, numElements1, numElements2);
  setParameter(object, name, ANARI_ARRAY2D, &array);
  *elementStride = anari::sizeOf(dataType);
  bool paramExists = AnariToUsdObjectPtr(array)->useCount() > 1;
  AnariToUsdObjectPtr(array)->refDec(helium::RefType::PUBLIC);
  return paramExists ? mapArray(array) : nullptr;
}

void *UsdDevice::mapParameterArray3D(ANARIObject object,
    const char *name,
    ANARIDataType dataType,
    uint64_t numElements1,
    uint64_t numElements2,
    uint64_t numElements3,
    uint64_t *elementStride)
{
  auto array = newArray3D(nullptr,
      nullptr,
      nullptr,
      dataType,
      numElements1,
      numElements2,
      numElements3);
  setParameter(object, name, ANARI_ARRAY3D, &array);
  *elementStride = anari::sizeOf(dataType);
  bool paramExists = AnariToUsdObjectPtr(array)->useCount() > 1;
  AnariToUsdObjectPtr(array)->refDec(helium::RefType::PUBLIC);
  return paramExists ? mapArray(array) : nullptr;
}

void UsdDevice::unmapParameterArray(ANARIObject object, const char *name)
{
  if(!object)
    return;

  ANARIDataType paramType = ANARI_UNKNOWN;
  void* paramAddress = getBaseObjectPtr(object)->getParameter(name, paramType);

  if(paramAddress && anari::isArray(paramType))
  {
    auto arrayAddress = (ANARIArray*)paramAddress;
    if(*arrayAddress)
      AnariToUsdObjectPtr(*arrayAddress)->unmap(this);
  }
}

void UsdDevice::release(ANARIObject object)
{
  if(!object)
    return;

  UsdBaseObject* baseObject = getBaseObjectPtr(object);

  bool privatizeArray = anari::isArray(baseObject->getType())
    && baseObject->useCount(helium::RefType::INTERNAL) > 0
    && baseObject->useCount(helium::RefType::PUBLIC) == 1;

#ifdef CHECK_MEMLEAKS
  if(!handleIsDevice(object))
    logObjDeallocation(baseObject);
#endif

  if (baseObject)
    baseObject->refDec(helium::RefType::PUBLIC);

  if (privatizeArray)
    AnariToUsdObjectPtr((ANARIArray)object)->privatize();
}

void UsdDevice::retain(ANARIObject object)
{
  if(object)
    getBaseObjectPtr(object)->refInc(helium::RefType::PUBLIC);
}

void UsdDevice::commitParameters(ANARIObject object)
{
  if(object)
    getBaseObjectPtr(object)->commit(this);
}

#ifdef CHECK_MEMLEAKS
namespace
{
  template<typename T>
  void SharedLogDeallocation(const T* ptr, std::vector<const T*>& allocations, UsdDevice* device)
  {
    if (ptr)
    {
      auto it = std::find(allocations.begin(), allocations.end(), ptr);
      if(it == allocations.end())
      {
        std::stringstream errstream;
        errstream << "USD Device release of nonexisting or already released/deleted object: 0x" << std::hex << ptr;

        device->reportStatus(device, ANARI_DEVICE, ANARI_SEVERITY_FATAL_ERROR, ANARI_STATUS_INVALID_OPERATION, errstream.str().c_str());
      }

      if(ptr->useCount() == 1)
      {
        assert(it != allocations.end());
        allocations.erase(it);
      }
    }
  }

  template<typename T>
  bool isAllocated(const T* ptr, const std::vector<const T*>& allocations)
  {
    auto it = std::find(allocations.begin(), allocations.end(), ptr);
    return it != allocations.end();
  }
}

void UsdDevice::logObjAllocation(const UsdBaseObject* ptr)
{
  allocatedObjects.push_back(ptr);
}

void UsdDevice::logObjDeallocation(const UsdBaseObject* ptr)
{
  SharedLogDeallocation(ptr, allocatedObjects, this);
}

void UsdDevice::logStrAllocation(const UsdSharedString* ptr)
{
  allocatedStrings.push_back(ptr);
}

void UsdDevice::logStrDeallocation(const UsdSharedString* ptr)
{
  SharedLogDeallocation(ptr, allocatedStrings, this);
}

void UsdDevice::logRawAllocation(const void* ptr)
{
  allocatedRawMemory.push_back(ptr);
}

void UsdDevice::logRawDeallocation(const void* ptr)
{
  if (ptr)
  {
    auto it = std::find(allocatedRawMemory.begin(), allocatedRawMemory.end(), ptr);
    if(it == allocatedRawMemory.end())
    {
      std::stringstream errstream;
      errstream << "USD Device release of nonexisting or already released/deleted raw memory: 0x" << std::hex << ptr;

      reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_FATAL_ERROR, ANARI_STATUS_INVALID_OPERATION, errstream.str().c_str());
    }
    else
      allocatedRawMemory.erase(it);
  }
}

bool UsdDevice::isObjAllocated(const UsdBaseObject* ptr) const
{
  return isAllocated(ptr, allocatedObjects);
}

bool UsdDevice::isStrAllocated(const UsdSharedString* ptr) const
{
  return isAllocated(ptr, allocatedStrings);
}

bool UsdDevice::isRawAllocated(const void* ptr) const
{
  return isAllocated(ptr, allocatedRawMemory);
}
#endif

