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
#include "UsdDevice_queries.h"
#include "UsdBridge/UsdBridgeMemoryStore.h"
#include "UsdBridge/xxhash/xxhash.h"
#include "UsdBridge/UsdBridgeDiffCapture.h"

#include "UsdBridge/Common/UsdBridgeParallelController.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <set>
#include <memory>
#include <sstream>
#include <algorithm>
#include <limits>
#include <filesystem>
#include <thread>
#include <atomic>
#include <system_error>
#include <chrono>

#ifdef USD_DEVICE_MPI_ENABLED
#include "UsdMpiController.h"
#endif

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
      deviceParams.outputMdlShader,
      deviceParams.useDisplayColorOpacity
    };

#ifdef USD_DEVICE_MPI_ENABLED
    if(!mpiController)
      mpiController = UsdMpiController::CreateDefault();
#endif

    if(mpiController)
    {
      bridgeSettings.MpiRank = mpiController->GetRank();
      bridgeSettings.MpiSize = mpiController->GetSize();
      bridgeSettings.ParallelController = mpiController.get();
    }

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
  bool enableSaving = false;
  std::unique_ptr<UsdBridge> bridge;
  SceneStagePtr externalSceneStage{nullptr};

  // MPI parallel support (KHR_DATA_PARALLEL_MPI)
  std::unique_ptr<UsdBridgeParallelController> mpiController;

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
  REGISTER_PARAMETER_MACRO("usd::output.displayColorOpacity", ANARI_BOOL, useDisplayColorOpacity)
  REGISTER_PARAMETER_MACRO("usd::autoFlush", ANARI_BOOL, autoFlushOnGeometryCommit)
)

void UsdDevice::clearDeviceParameters()
{
  filterResetParam("usd::serialize.hostName");
  filterResetParam("usd::serialize.location");
  transferWriteToReadParams();
}
//----

UsdDevice::UsdDevice()
  : UsdParameterizedBaseObject<UsdDevice, UsdDeviceData>(ANARI_DEVICE)
  , internals(std::make_unique<UsdDeviceInternals>())
{}

UsdDevice::UsdDevice(ANARILibrary library)
  : DeviceImpl(library)
  , UsdParameterizedBaseObject<UsdDevice, UsdDeviceData>(ANARI_DEVICE)
  , internals(std::make_unique<UsdDeviceInternals>())
{}

UsdDevice::~UsdDevice()
{
  // Make sure no more references are held before cleaning up the device (and checking for memleaks)
  clearCommitList(); 

  clearDeviceParameters(); // Release device parameters with object references

  clearResourceStringList(); // Do the same for resource string references

  //internals->bridge->SaveScene(); //Uncomment to test cleanup of usd files.

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
  reportStatus(source, sourceType, severity, statusCode, format, &arglist);
  va_end(arglist);
}

static void reportBridgeStatus(UsdBridgeLogLevel level, void* device, const char *message)
{
  ANARIStatusSeverity severity = UsdBridgeLogLevelToAnariSeverity(level);

  ((UsdDevice*)device)->reportStatus(nullptr, ANARI_UNKNOWN, severity, ANARI_STATUS_NO_ERROR, message, nullptr);
}

void UsdDevice::reportStatus(void* source,
  ANARIDataType sourceType,
  ANARIStatusSeverity severity,
  ANARIStatusCode statusCode,
  const char *format,
  va_list* arglist)
{
  // Use a thread-local buffer: USD diagnostic callbacks may fire from any thread,
  // so a shared member vector would cause data races.
  thread_local std::vector<char> messageBuffer;

  if(arglist)
  {
    va_list arglist_copy;
    va_copy(arglist_copy, *arglist);
    int count = std::vsnprintf(nullptr, 0, format, *arglist);

    messageBuffer.resize(count + 1);

    std::vsnprintf(messageBuffer.data(), count + 1, format, arglist_copy);
    va_end(arglist_copy);
  }
  else
  {
    int count = static_cast<int>(strlen(format));
    messageBuffer.resize(count + 1);
    std::memcpy(messageBuffer.data(), format, count + 1);
  }

  if (statusFunc != nullptr)
  {
    statusFunc(
      statusUserData,
      (ANARIDevice)this,
      (ANARIObject)source,
      sourceType,
      severity,
      statusCode,
      messageBuffer.data());
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
    // SAFETY: Respect disableGarbageCollect flag
    if (disableGarbageCollect_)
    {
      reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_WARNING, ANARI_STATUS_NO_ERROR,
        "USDRMNG: usd::garbageCollect blocked — disableGarbageCollect is true");
      return;
    }
    // Perform garbage collection on usd objects (needs to move into the user interface)
    if(internals->bridge)
      internals->bridge->GarbageCollect();
  }
  else if(strEquals(name, "usd::disableGarbageCollect"))
  {
    // SAFETY: Disable GC to prevent orphaned prim cleanup from removing scene objects prematurely
    disableGarbageCollect_ = true;
    reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO, ANARI_STATUS_NO_ERROR,
      "USDRMNG: usd::disableGarbageCollect = true — GC disabled to prevent missing actors");
  }
  else if(strEquals(name, "usd::debug.sceneSummary"))
  {
    // DEBUG: Dump full scene summary to logs
    if(internals->bridge)
    {
      std::stringstream ss;
      ss << "USDRMNG: === SCENE SUMMARY ===\n";
      
      // Commit list status
      ss << "CommitList: " << commitList.size() << " pending objects\n";
      ss << "RemoveList: " << removeList.size() << " marked for removal\n";
      
      ss << "Worlds: " << commitList.size() << " objects in commit list\n";
       for (const auto& entry : commitList)
       {
         ss << "  - [" << (int)entry.first.ptr->getType() << "] " << entry.first.ptr->getType()
            << " commitData=" << entry.second << "\n";
       }
      
      // Memory store
      if (g_rankMemoryStore)
      {
        auto files = g_rankMemoryStore->ListFiles();
        ss << "MemoryStore: " << files.size() << " files\n";
        for (const auto& f : files)
        {
          auto fe = g_rankMemoryStore->GetFile(f);
          ss << "  - " << f << " (" << (fe ? fe->size() : 0) << " bytes)\n";
        }
      }
      
      // Remove list
      if (!removeList.empty())
      {
        ss << "RemoveList objects:\n";
        for (auto* obj : removeList)
          ss << "  - [" << (int)obj->getType() << "] " << obj->getType() << "\n";
      }
      
      reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO, ANARI_STATUS_NO_ERROR, "%s", ss.str().c_str());
    }
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
  else if(strEquals(name, "mpiCommunicator"))
  {
    if(type == ANARI_VOID_POINTER)
    {
#ifdef USD_DEVICE_MPI_ENABLED
      if(mem)
        internals->mpiController = std::make_unique<UsdMpiController>(mem);
      else
        internals->mpiController.reset();
#else
      reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_WARNING, ANARI_STATUS_INVALID_OPERATION,
        "mpiCommunicator parameter set but USD device was not built with MPI support (USD_DEVICE_MPI_ENABLED)");
#endif
    }
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
  else if (strEquals(name, "mpiCommunicator"))
  {
    internals->mpiController.reset();
  }
  else if (!strEquals(name, "usd::garbageCollect")
    && !strEquals(name, "usd::disableGarbageCollect")
    && !strEquals(name, "usd::debug.sceneSummary")
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

  const UsdDeviceData& paramData = getReadParams();
  internals->bridge->UpdateBeginEndTime(paramData.timeStep);
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

  // Geometry-commit auto-flush removed — USD + ZMQ now only triggered by renderFrame (camera pan)
  reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO, ANARI_STATUS_NO_ERROR,
      "usd::flush = renderFrame only (camera pan triggers USD save + ZMQ)");

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
      internals->outputLocation = "memory://";
    }
  }

  if(internals->mpiController)
  {
    reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO, ANARI_STATUS_NO_ERROR,
      "MPI rank %d/%d: output to '%s'",
      internals->mpiController->GetRank(), internals->mpiController->GetSize(),
      internals->outputLocation.c_str());
  }

  if (!internals->CreateNewBridge(paramData, &reportBridgeStatus, this))
  {
    reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_ERROR, ANARI_STATUS_UNKNOWN_ERROR, "Usd Bridge failed to load");
    return;
  }

  // MPI rank detection from environment variables
  #ifdef ANARI_USD_ENABLE_MPI
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
    if (mpiRank == 0) {
      zmqBroker_ = std::make_unique<usd_bridge::ZmqBroker>(5555);
      if (!zmqBroker_->Initialize(mpiSize - 1)) {
        reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_WARNING, ANARI_STATUS_UNKNOWN_ERROR,
            "Failed to initialize ZMQ broker on rank 0");
        zmqBroker_.reset();
      }
      if (zmqBroker_) {
        const auto& workers = zmqBroker_->GetConnectedWorkers();
        for (const auto& worker : workers) {
          reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO, ANARI_STATUS_NO_ERROR,
              "Rank 0: Worker %d connected from %s", worker.rank, worker.hostname.c_str());
        }
        fileServingActive_ = true;
        fileServingThread_ = std::thread(&UsdDevice::FileServingThreadLoop, this);
        reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO,
                     ANARI_STATUS_NO_ERROR,
                     "Rank 0: Started background file serving thread");
      }
    }
    else {
      zmqWorker_ = std::make_unique<usd_bridge::ZmqWorker>("", mpiRank);
      if (zmqWorker_->Connect()) {
        fileServingActive_ = true;
        fileServingThread_ = std::thread(&UsdDevice::FileServingThreadLoop, this);
        reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO,
                     ANARI_STATUS_NO_ERROR,
                     "Rank %d: Started background file serving thread", mpiRank);
      }
      else {
        reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_WARNING,
                     ANARI_STATUS_UNKNOWN_ERROR,
                     "Failed to connect to broker on rank %d, continuing without ZMQ", mpiRank);
        zmqWorker_.reset();
      }
    }
    InitializeMemoryStore(mpiRank);
    reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO,
                 ANARI_STATUS_NO_ERROR,
                 "Memory store initialized for rank %d", mpiRank);
  }
  else if (mpiAvailable) {
    // Single-rank MPI: still use IB IP for broker
    zmqBroker_ = std::make_unique<usd_bridge::ZmqBroker>(5555);
    if (!zmqBroker_->Initialize(0)) {
      reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_WARNING, ANARI_STATUS_UNKNOWN_ERROR,
          "Failed to initialize ZMQ broker, continuing without ZMQ");
      zmqBroker_.reset();
    }
    else {
      reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO, ANARI_STATUS_NO_ERROR,
          "Single-rank MPI ZMQ broker started (rank %d, using IB IP)", mpiRank);
    }
    InitializeMemoryStore(mpiRank);
    reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO,
                 ANARI_STATUS_NO_ERROR,
                 "Memory store initialized for rank %d", mpiRank);
  }
  else {
    zmqBroker_ = std::make_unique<usd_bridge::ZmqBroker>(5555);
    if (!zmqBroker_->Initialize(0)) {
      reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_WARNING, ANARI_STATUS_UNKNOWN_ERROR,
          "Failed to initialize ZMQ broker, continuing without ZMQ");
      zmqBroker_.reset();
    }
    else {
      reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO, ANARI_STATUS_NO_ERROR,
          "Non-MPI ZMQ broker started");
    }
    InitializeMemoryStore(0);
    reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO,
                 ANARI_STATUS_NO_ERROR,
                 "Memory store initialized for local file serving");
  }
  #else
  zmqBroker_ = std::make_unique<usd_bridge::ZmqBroker>(5555);
  if (!zmqBroker_->Initialize(0)) {
    reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_WARNING, ANARI_STATUS_UNKNOWN_ERROR,
        "Failed to initialize ZMQ broker, continuing without ZMQ");
    zmqBroker_.reset();
  }
  else {
    reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO, ANARI_STATUS_NO_ERROR,
        "Non-MPI ZMQ broker started");
  }
  InitializeMemoryStore(0);
  reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO,
               ANARI_STATUS_NO_ERROR,
               "Memory store initialized for local file serving");
  #endif
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

  ANARICamera returnValue = (ANARICamera)(object);

  return returnValue;
}

ANARIObject UsdDevice::newObject(const char *objectType, const char *type)
{
  return nullptr;
}

void (*UsdDevice::getProcAddress(const char *name))(void)
{
  return nullptr;
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
  if (!type || (!strEquals(type, "default") && !strEquals(type, "hydra")))
  {
    reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_DEBUG, ANARI_STATUS_INVALID_ARGUMENT,
      "Unrecognized renderer subtype '%s'. Supported subtypes: 'default', 'hydra'.", type ? type : "(null)");
    return nullptr;
  }

  UsdRenderer* object = new UsdRenderer(type);
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

  // Log pre-flush state
  if (!commitList.empty())
  {
    reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO, ANARI_STATUS_NO_ERROR,
      "USDRMNG: renderFrame — flushing %zu pending objects", commitList.size());
  }

  flushCommitList();

  // Ensure device usd::time is committed before USD save — this guarantees
  // the bridge processes data at the correct timestep, preventing missing actors.
  // This is required when the user does usd::writeAtCommit = false (default).
  transferWriteToReadParams();
  const UsdDeviceData& paramData = getReadParams();
  internals->bridge->UpdateBeginEndTime(paramData.timeStep);

  internals->bridge->ResetResourceUpdateState(); // Reset the modified flags for committed shared resources

  if(frame)
  {
    UsdFrame* frameObjPtr = AnariToUsdObjectPtr(frame);
    
    // Debug: log pre-save file state
    if (g_rankMemoryStore)
    {
      auto files = g_rankMemoryStore->ListFiles();
      reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO, ANARI_STATUS_NO_ERROR,
        "USDRMNG: renderFrame — MemoryStore has %zu files before save", files.size());
    }
    
    frameObjPtr->saveUsd(this);
    
    // Debug: log post-save file state
    if (g_rankMemoryStore)
    {
      auto files = g_rankMemoryStore->ListFiles();
      reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO, ANARI_STATUS_NO_ERROR,
        "USDRMNG: renderFrame — MemoryStore has %zu files after save", files.size());
    }
    
    frameObjPtr->renderFrame(this);
  }

  // Send ZMQ notifications to laptop client (only from rank 0 to avoid duplicates)
  #ifdef ANARI_USD_ENABLE_MPI
  if (zmqWorker_ && zmqWorker_->IsConnected() && frame) {
    uint64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // 1. First: notify ONLY about files that were actually written this renderFrame cycle.
    //    DiffCapture holds entries for files whose CapturePreStore() was called right before
    //    StoreFile(). Iterating DiffCapture instead of MemoryStore.ListFiles() avoids sending
    //    stale V2 notifications for clips from previous frames (rank redistribution left them
    //    in the store but they weren't written this cycle).
    auto& diffCap = GetDiffCapture();
    auto capturedFiles = diffCap.GetCapturedFilenames();

    if (capturedFiles.empty()) {
      // Fallback: if DiffCapture is empty but MemoryStore has clips,
      // still notify them (first frame after connection, no old hashes yet).
      // These are genuinely new files — the client has never seen them.
      if (g_rankMemoryStore) {
        auto allFiles = g_rankMemoryStore->ListFiles();
        for (const auto& name : allFiles) {
          if (name.find("clips/") == 0 || name.find("images/") == 0) {
            auto entry = g_rankMemoryStore->GetFile(name);
            if (entry) {
              zmqWorker_->SendFileNotificationV2(name, entry->data.size(), timestamp, entry->hash128, nullptr, false);
            }
          }
        }
      }
    } else {
      // Only V2-notify files that were actually written this frame
      for (const auto& name : capturedFiles) {
        // Only geometry and image files get V2 notifications
        if (name.find("clips/") != 0 && name.find("images/") != 0) {
          diffCap.Commit(name);
          continue;
        }
        auto entry = g_rankMemoryStore ? g_rankMemoryStore->GetFile(name) : nullptr;
        if (entry) {
          const uint64_t* oldHash = diffCap.GetOldHash128(name);
          bool hasOld = diffCap.HasOldEntry(name);
          zmqWorker_->SendFileNotificationV2(name, entry->data.size(), timestamp, entry->hash128, oldHash, hasOld);
        }
        diffCap.Commit(name);
      }
    }

    // 2. Barrier: wait for all ranks to finish sending their file notifications
    //    This ensures CommitComplete arrives AFTER all V2 file notifications,
    //    so the UE client's dedup sets are cleared at the right time.
#ifdef USD_DEVICE_MPI_ENABLED
    if (internals->mpiController) {
      internals->mpiController->Barrier();
    }
#endif

    // 3. Last: only rank 0 sends the commit notification to avoid 16 duplicates
    if (mpiRank == 0) {
      const char* frameFilename = "FullScene.usda";
      auto fileEntry = g_rankMemoryStore ? g_rankMemoryStore->GetFile(frameFilename) : nullptr;
      uint64_t fileSize = fileEntry ? fileEntry->data.size() : 0;
      const uint64_t* fileHash = fileEntry ? fileEntry->hash128 : nullptr;
      zmqWorker_->SendCommitNotification(frameFilename, fileSize, timestamp, fileHash);
    }
  }

  // Serve file requests from laptop client
  if (zmqWorker_ && zmqWorker_->IsConnected()) {
    ServeFileRequests();
  }
  #else
  if (zmqBroker_) {
    ServeFileRequests();
  }
  #endif
}

int UsdDevice::frameReady(ANARIFrame frame, ANARIWaitMask mask)
{
  if(!isInitialized())
    return 1;

  if(frame)
  {
    UsdFrame* frameObjPtr = AnariToUsdObjectPtr(frame);
    return frameObjPtr->frameReady(mask, this);
  }
  return 1;
}

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
    else
      it->second = it->second && commitData;
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

  // Log commit list summary before processing
  size_t totalCount = commitList.size();
  size_t perTypeCounts[16] = {0}; // Track per-type counts
  for (const auto& entry : commitList)
  {
    int typeIdx = (int)entry.first.ptr->getType();
    if (typeIdx >= 0 && typeIdx < (int)(sizeof(perTypeCounts)/sizeof(perTypeCounts[0])))
      perTypeCounts[typeIdx]++;
  }
  std::stringstream flushLog;
  flushLog << "USDRMNG: flushCommitList — total=" << totalCount << " objects: ";
  if (perTypeCounts[1]) flushLog << "[" << perTypeCounts[1] << " sampler] ";
  if (perTypeCounts[4]) flushLog << "[" << perTypeCounts[4] << " geometry] ";
  if (perTypeCounts[2]) flushLog << "[" << perTypeCounts[2] << " spatialField] ";
  if (perTypeCounts[15]) flushLog << "[" << perTypeCounts[15] << " light] ";
  if (perTypeCounts[12]) flushLog << "[" << perTypeCounts[12] << " material] ";
  if (perTypeCounts[11]) flushLog << "[" << perTypeCounts[11] << " surface] ";
  if (perTypeCounts[13]) flushLog << "[" << perTypeCounts[13] << " volume] ";
  if (perTypeCounts[5]) flushLog << "[" << perTypeCounts[5] << " group] ";
  if (perTypeCounts[6]) flushLog << "[" << perTypeCounts[6] << " instance] ";
  if (perTypeCounts[8]) flushLog << "[" << perTypeCounts[8] << " world] ";
  if (perTypeCounts[9]) flushLog << "[" << perTypeCounts[9] << " camera] ";
  if (perTypeCounts[10]) flushLog << "[" << perTypeCounts[10] << " frame] ";
  if (perTypeCounts[16]) flushLog << "[" << perTypeCounts[16] << " renderer] ";
  reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO, ANARI_STATUS_NO_ERROR, "%s", flushLog.str().c_str());

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
  writeTypeToUsd<(int)ANARI_FRAME>();

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
  if(it != volumeList.end())
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
  if(onlyRemoveHandles)
  {
    if (!removeList.empty())
      reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO, ANARI_STATUS_NO_ERROR,
        "USDRMNG: removePrimsFromUsd — handle-only pass, skipping removal of %zu objects", removeList.size());
  }
  else
  {
    if (!removeList.empty())
    {
      reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_WARNING, ANARI_STATUS_NO_ERROR,
        "USDRMNG: removePrimsFromUsd — actively removing %zu prims marked with usd::removePrim", removeList.size());
      for (auto baseObj : removeList)
      {
        reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_WARNING, ANARI_STATUS_NO_ERROR,
          "USDRMNG: removePrimsFromUsd — removing prim of type %d",
          (int)baseObj->getType());
        baseObj->remove(this);
      }
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
    else if (strEquals(name, "workerCount") && type == ANARI_INT32) {
      int32_t workerCount = 0;
      #ifdef ANARI_USD_ENABLE_MPI
      if (mpiAvailable && mpiRank == 0 && zmqBroker_) {
        workerCount = static_cast<int32_t>(zmqBroker_->GetConnectedWorkers().size());
      }
      else if (mpiAvailable && mpiSize > 1) {
        workerCount = 0;
      }
      #endif
      writeToVoidP(mem, workerCount);
      return 1;
    }
    else if (strEquals(name, "mpiSize") && type == ANARI_INT32) {
      int32_t mpiSizeValue = 1;
      #ifdef ANARI_USD_ENABLE_MPI
      if (mpiAvailable) {
        mpiSizeValue = mpiSize;
      }
      #endif
      writeToVoidP(mem, mpiSizeValue);
      return 1;
    }
    else if (strEquals(name, "mpiRank") && type == ANARI_INT32) {
      int32_t mpiRankValue = 0;
      #ifdef ANARI_USD_ENABLE_MPI
      if (mpiAvailable) {
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
  const char* name = makeUniqueName("Frame");
  UsdFrame* object = new UsdFrame(name, this);
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
    return AnariToUsdObjectPtr(fb)->mapBuffer(channel, width, height, pixelType, this);
  return nullptr;
}

void UsdDevice::frameBufferUnmap(ANARIFrame fb, const char *channel)
{
  if (fb)
    return AnariToUsdObjectPtr(fb)->unmapBuffer(channel, this);
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
  {
    getBaseObjectPtr(object)->commit(this);
  }
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

#ifdef ANARI_USD_ENABLE_MPI
void UsdDevice::ServeFileRequests()
{
  if (mpiAvailable && mpiRank == 0) return;
  if (!zmqWorker_) return;

  using namespace usd_bridge;
  ZmqFileRequest request;

  while (zmqWorker_->CheckForFileRequest(request, false)) {
    if (request.magic != USD_FILE_MAGIC) {
      reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_WARNING,
                   ANARI_STATUS_INVALID_OPERATION,
                   "Rank %d: Received invalid file request (bad magic)", mpiRank);
      continue;
    }

    if (request.message_type == static_cast<uint32_t>(ZmqMessageType::REQ_LIST_FILES)) {
      auto files = g_rankMemoryStore ? g_rankMemoryStore->ListFiles() : std::vector<std::string>();
      std::stringstream jsonResponse;
      jsonResponse << "{\"rank\":" << mpiRank << ",\"files\":[";
      bool first = true;
        for (const auto& filename : files) {
         if (filename.find(".usda.usda") != std::string::npos) continue;
         if (!first) jsonResponse << ",";
        uint64_t fsize = 0;
        const char* mime = "application/octet-stream";
        uint64_t hLo = 0, hHi = 0;
        if (g_rankMemoryStore) {
           auto entry = g_rankMemoryStore->GetFile(filename);
          if (entry) {
            fsize = entry->size();
            mime = entry->mime_type.c_str();
            hLo = entry->hash128[0];
            hHi = entry->hash128[1];
            if (hLo == 0 && hHi == 0) { XXH128_hash_t h = XXH3_128bits(entry->data.data(), entry->data.size()); hLo = h.low64; hHi = h.high64; }
          }
        }
        jsonResponse << "{\"name\":\"" << filename << "\",\"size\":" << fsize << ",\"mime\":\"" << mime << "\",\"hash_lo\":" << hLo << ",\"hash_hi\":" << hHi << "}";
        first = false;
      }
      jsonResponse << "]}";
      std::string jsonStr = jsonResponse.str();
      zmqWorker_->SendFileChunk(request.request_id, "__file_list__.json",
          jsonStr.data(), jsonStr.size(), jsonStr.size(), 0);
      zmqWorker_->SendFileComplete(request.request_id, "__file_list__.json", jsonStr.size());
      continue;
    }

    if (request.message_type == static_cast<uint32_t>(ZmqMessageType::REQ_GET_FRAME)) {
      // Frame = every file currently in this rank's store, streamed under the
      // single request_id, and terminated by a ZmqFileComplete whose filename
      // is "__frame_complete__" (same marker convention the rank-0 broker
      // uses; reuses existing message types, so no protocol change).
      if (g_rankMemoryStore) {
        const size_t kMaxChunkSize = 128 * 1024 * 1024;
        size_t frameChunkSize = (request.chunk_size > 0)
            ? std::min<size_t>(request.chunk_size, kMaxChunkSize)
            : DEFAULT_CHUNK_SIZE;
        int filesSent = 0;
        for (const auto& filename : g_rankMemoryStore->ListFiles()) {
          if (filename.find(".usda.usda") != std::string::npos) continue;
          auto entry = g_rankMemoryStore->GetFile(filename);
          if (!entry) continue;
          const size_t total = entry->data.size();
          uint64_t off = 0;
          while (off < total) {
            size_t sendSize = std::min(frameChunkSize, total - off);
            zmqWorker_->SendFileChunk(request.request_id, filename,
                entry->data.data() + off, sendSize, total, off);
            off += sendSize;
          }
          zmqWorker_->SendFileComplete(request.request_id, filename, total);
          ++filesSent;
        }
        // Terminal marker: the broker keeps the client mapping alive until this
        // arrives, then drops it (see frame_requests_ in the broker loop).
        zmqWorker_->SendFileComplete(request.request_id, "__frame_complete__",
            static_cast<uint64_t>(filesSent));
      }
      continue;
    }

    if (!g_rankMemoryStore) {
      zmqWorker_->SendNoFile(request.request_id, request.filename);
      continue;
    }

    auto fileEntry = g_rankMemoryStore->GetFile(request.filename);
    if (!fileEntry) {
      zmqWorker_->SendNoFile(request.request_id, request.filename);
      continue;
    }

    size_t totalSize = fileEntry->data.size();
    // Honor the client's preferred chunk size (larger chunks = fewer ZMQ
    // round-trips through the broker loop). Fall back to the default, and cap
    // at 128MB against a malformed request.
    const size_t kMaxChunkSize = 128 * 1024 * 1024;
    size_t chunkSize = (request.chunk_size > 0)
        ? std::min<size_t>(request.chunk_size, kMaxChunkSize)
        : DEFAULT_CHUNK_SIZE;
    size_t offset = 0;

    while (offset < totalSize) {
      size_t sendSize = std::min(chunkSize, totalSize - offset);
      zmqWorker_->SendFileChunk(request.request_id, request.filename,
          fileEntry->data.data() + offset, sendSize, totalSize, offset);
      offset += sendSize;
    }

    if (offset == totalSize) {
      zmqWorker_->SendFileComplete(request.request_id, request.filename, totalSize);
    }
  }
}

void UsdDevice::FileServingThreadLoop()
{
  while (fileServingActive_) {
    ServeFileRequests();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

void UsdDevice::NotifyFrameReady(double timestep)
{
  (void)timestep;
}
#endif

// -----------------------------------------------------------------------------
// Event-driven flush on geometry commit
// Called from commitParameters() when ParaView commits geometry data.
// Debounced: only fires once per 500ms window to batch multiple geometry commits.
// -----------------------------------------------------------------------------

void UsdDevice::FlushSceneAndNotify()
{
  if (!isInitialized()) return;

  // Debounce: skip if we flushed recently (ParaView commits multiple objects per frame)
  auto now = std::chrono::steady_clock::now();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFlushTime_).count();
  if (elapsed < 500) {
    // Log the debounce skip to help diagnosis
    reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO, ANARI_STATUS_NO_ERROR,
      "USDRMNG: FlushSceneAndNotify — debounced (%ldms < 500ms), will flush in %ldms",
      (long)elapsed, (long)(500 - elapsed));
    return;
  }
  lastFlushTime_ = now;

  // Flush pending commit list
  if (!commitList.empty()) {
    reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO, ANARI_STATUS_NO_ERROR,
      "USDRMNG: FlushSceneAndNotify — flushing %zu pending objects", commitList.size());
    flushCommitList();
  }

  // Save USD scene to disk
  internals->bridge->ResetResourceUpdateState();
  internals->bridge->SaveScene();

  // Debug: log MemoryStore file count
  if (g_rankMemoryStore)
  {
    auto files = g_rankMemoryStore->ListFiles();
    reportStatus(this, ANARI_DEVICE, ANARI_SEVERITY_INFO, ANARI_STATUS_NO_ERROR,
      "USDRMNG: FlushSceneAndNotify — flush complete, MemoryStore has %zu files", files.size());
  }

#ifdef ANARI_USD_ENABLE_MPI
  // Send ZMQ notifications
  if (zmqWorker_ && zmqWorker_->IsConnected()) {
    uint64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // 1. First: notify about clip files and image files that changed on this rank
    if (g_rankMemoryStore) {
      auto allFiles = g_rankMemoryStore->ListFiles();
      for (const auto& name : allFiles) {
        if (name.find("clips/") == 0 || name.find("images/") == 0) {
          auto entry = g_rankMemoryStore->GetFile(name);
          if (entry) {
            // DIFF-CAPTURE-HOOK: send old-hash-aware notification
            const uint64_t* oldHash = GetDiffCapture().GetOldHash128(name);
            bool hasOld = GetDiffCapture().HasOldEntry(name);
            zmqWorker_->SendFileNotificationV2(name, entry->data.size(), timestamp, entry->hash128, oldHash, hasOld);
            GetDiffCapture().Commit(name);
          }
        }
      }
    }

    // 2. Barrier: wait for all ranks to finish sending their file notifications
    //    This ensures CommitComplete arrives AFTER all V2 file notifications,
    //    so the UE client's dedup sets are cleared at the right time.
#ifdef USD_DEVICE_MPI_ENABLED
    if (internals->mpiController) {
      internals->mpiController->Barrier();
    }
#endif

    // 3. Last: only rank 0 sends commit notification to avoid duplicates
    if (mpiRank == 0 && g_rankMemoryStore) {
      auto fileEntry = g_rankMemoryStore->GetFile("FullScene.usda");
      uint64_t fileSize = fileEntry ? fileEntry->data.size() : 0;
      const uint64_t* fileHash = fileEntry ? fileEntry->hash128 : nullptr;
      zmqWorker_->SendCommitNotification("FullScene.usda", fileSize, timestamp, fileHash);
    }

    // Serve any pending file requests
    ServeFileRequests();
  }
#endif
}