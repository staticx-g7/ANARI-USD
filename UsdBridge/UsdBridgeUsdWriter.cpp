// Copyright 2020 The Khronos Group

#include "UsdBridgeUsdWriter.h"
#include "xxhash/xxhash.h"
#include "UsdBridgeCaches.h"
#include "UsdBridgeMdlStrings.h"
#include "UsdBridgeUsdWriter_Common.h"
#include "UsdBridgeDiagnosticMgrDelegate.h"
#include "UsdBridgeMemoryStore.h"
#include "UsdBridgeDiffCapture.h"
#include "../AnariUsdProtocol.h" // JusyncRawGeoHeader wire format (shared with the JUSYNC client)
#include "RawGeoCurvePack.h"     // RawGeo v3 curve subset packer
#include "RawGeoParticleId.h"    // stable point-cloud IDs as exact float chunks
#include "Common/UsdBridgeParallelController.h"

#include <cstring>
#include <vector>

#include <pxr/usd/usdRender/product.h>
#include <pxr/usd/usdRender/settings.h>

#include <filesystem>
#include <set>
#include <typeinfo>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <atomic>
#include "pxr/usd/sdf/fileFormat.h"

#define PROCESS_PREFIX

TF_DEFINE_PUBLIC_TOKENS(
  UsdBridgeTokens,

  ATTRIB_TOKEN_SEQ
  USDPREVSURF_TOKEN_SEQ
  USDPREVSURF_INPUT_TOKEN_SEQ
  MDL_TOKEN_SEQ
  MDL_INPUT_TOKEN_SEQ
  VOLUME_TOKEN_SEQ
  INDEX_TOKEN_SEQ
  MISC_TOKEN_SEQ
);

TF_DEFINE_PUBLIC_TOKENS(
  UsdBridgeRenderTokens,

  RENDER_TOKEN_SEQ
);

#undef PROCESS_PREFIX

namespace constring
{
  const char* const sessionPf = "Session_";
  const char* const rootClassName = "/RootClass";
  const char* const rootPrimName = "/Root";

  const char* const manifestFolder = "manifests/";
  const char* const clipFolder = "clips/";
  const char* const primStageFolder = "primstages/";
  const char* const imgFolder = "images/";
  const char* const volFolder = "volumes/";

  const char* const texCoordReaderPrimPf = "texcoordreader";
  const char* const psShaderPrimPf = "psshader";
  const char* const mdlShaderPrimPf = "mdlshader";
  const char* const psSamplerPrimPf = "pssampler";
  const char* const mdlSamplerPrimPf = "mdlsampler";
  const char* const mdlOpacityMulPrimPf = "opacitymul_mdl";
  const char* const mdlDiffuseOpacityPrimPf = "diffuseopacity_mdl";
  const char* const mdlGraphXYZPrimPf = "_xyz_f";
  const char* const mdlGraphColorPrimPf = "_ftocolor";
  const char* const mdlGraphXPrimPf = "_x";
  const char* const mdlGraphYPrimPf = "_y";
  const char* const mdlGraphZPrimPf = "_z";
  const char* const mdlGraphWPrimPf = "_w";
  const char* const openVDBPrimPf = "ovdbfield";
  const char* const protoShapePf = "proto_";

  const char* const imageExtension = ".png";
  const char* const vdbExtension = ".vdb";

  const char* const fullSceneNameBin = "FullScene.usd";
  const char* const fullSceneNameAscii = "FullScene.usda";

  const char* const parallelSceneNameBin = "ParallelScene.usda";
  const char* const parallelSceneNameAscii = "ParallelScene.usda";

  const char* const mdlShaderAssetName = "OmniPBR.mdl";
  const char* const mdlSupportAssetName = "nvidia/support_definitions.mdl";
  const char* const mdlAuxAssetName = "nvidia/aux_definitions.mdl";

#ifdef CUSTOM_PBR_MDL
  const char* const mdlFolder = "mdls/";

  const char* const opaqueMaterialFile = "PBR_Opaque.mdl";
  const char* const transparentMaterialFile = "PBR_Transparent.mdl";
#endif

#ifdef USE_INDEX_MATERIALS
  const char* const indexMaterialPf = "indexmaterial";
  const char* const indexShaderPf = "indexshader";
  const char* const indexColorMapPf = "indexcolormap";
#endif
}

static bool GetMpiRankSizeFromEnv(int &rank, int &size)
{
  const char *slurmprocid = getenv("SLURM_PROCID");
  const char *slurmntasks = getenv("SLURM_NTASKS");
  const char *ompirank = getenv("OMPI_COMM_WORLD_RANK");
  const char *ompisize = getenv("OMPI_COMM_WORLD_SIZE");
  const char *pmirank = getenv("PMI_RANK");
  const char *pmisize = getenv("PMI_SIZE");

  if (slurmprocid && slurmntasks) {
    rank = std::atoi(slurmprocid);
    size = std::atoi(slurmntasks);
    return true;
  } else if (ompirank && ompisize) {
    rank = std::atoi(ompirank);
    size = std::atoi(ompisize);
    return true;
  } else if (pmirank && pmisize) {
    rank = std::atoi(pmirank);
    size = std::atoi(pmisize);
    return true;
  }

  rank = 0;
  size = 1;
  return false;
}

#ifdef OMNIVERSE_CONNECTION_ENABLE
namespace
{
  void InitializeUsdPlugins(const UsdBridgeLogObject& logObj)
  {
    PlugRegistry& registry = PlugRegistry::GetInstance();

    PlugPluginPtrVector allPlugins = registry.GetAllPlugins();
    std::string usdPluginPath;

    for (auto& plugin : allPlugins)
    {
      std::string name = plugin->GetName();
      std::string path = plugin->GetPath();

      if (name == "usd")
      {
        usdPluginPath = path;
        UsdBridgeLogMacro(logObj, UsdBridgeLogLevel::STATUS, "Found USD plugin reference: " << name << " at " << path);
        break;
      }
    }

    if (!usdPluginPath.empty())
    {
      std::filesystem::path usdPath(usdPluginPath);
      std::filesystem::path installDir = usdPath.parent_path().parent_path();
      std::filesystem::path resolverPluginDir = installDir / "plugin" / "omni_usd_resolver" / "resources";

      std::string resolverPluginPath = resolverPluginDir.string();
      UsdBridgeLogMacro(logObj, UsdBridgeLogLevel::STATUS, "Attempting to register plugins from: " << resolverPluginPath);

      registry.RegisterPlugins(resolverPluginPath);
    }
    else
    {
      UsdBridgeLogMacro(logObj, UsdBridgeLogLevel::WARNING, "Could not find USD plugin to determine plugin directory structure");
    }

#ifndef NDEBUG
    PlugPluginPtrVector plugins = registry.GetAllPlugins();
    UsdBridgeLogMacro(logObj, UsdBridgeLogLevel::STATUS, "=== USD Plugins (Total: " << plugins.size() << ") ===");

    for (auto& plugin : plugins)
    {
      std::string pluginName = plugin->GetName();
      std::string pluginPath = plugin->GetPath();
      bool isLoaded = plugin->IsLoaded();
      UsdBridgeLogMacro(logObj, UsdBridgeLogLevel::STATUS, "Plugin: " << pluginName << " | Path: " << pluginPath << " | Loaded: " << (isLoaded ? "YES" : "NO"));
    }

    ArResolver& resolver = ArGetResolver();
    std::string resolverType = typeid(resolver).name();
    UsdBridgeLogMacro(logObj, UsdBridgeLogLevel::STATUS, "Active resolver type: " << resolverType);
#endif
  }
}
#endif

#define PROCESS_PREFIX(elem) AttributeTokens.push_back(UsdBridgeTokens->elem); // Converts any token sequence macro to add all tokens to list

UsdBridgeUsdWriter::UsdBridgeUsdWriter(const UsdBridgeSettings& settings)
  : Settings(settings)
  , VolumeWriter(Create_VolumeWriter(), std::mem_fn(&UsdBridgeVolumeWriterI::Release))
{
  ATTRIB_TOKEN_SEQ

  if(Settings.HostName)
    ConnectionSettings.HostName = Settings.HostName;
  if(Settings.OutputPath)
    ConnectionSettings.WorkingDirectory = Settings.OutputPath;
  FormatDirName(ConnectionSettings.WorkingDirectory);
}

#undef PROCESS_PREFIX // Reset the process prefix on the token sequence

UsdBridgeUsdWriter::~UsdBridgeUsdWriter()
{
}

void UsdBridgeUsdWriter::SetExternalSceneStage(UsdStageRefPtr sceneStage)
{
  this->ExternalSceneStage = sceneStage;
}

void UsdBridgeUsdWriter::SetEnableSaving(bool enableSaving)
{
  this->EnableSaving = enableSaving;
}

void UsdBridgeUsdWriter::SetSelectiveFileSaving(bool selectiveFileSaving)
{
  this->SelectiveFileSaving = selectiveFileSaving;
}

void UsdBridgeUsdWriter::SaveScene()
{
  if(this->EnableSaving)
    this->SceneStage->Save();
}

int UsdBridgeUsdWriter::FindSessionNumber()
{
  int sessionNr = Connect->MaxSessionNr();

  sessionNr = std::max(0, sessionNr + Settings.CreateNewSession);

  return sessionNr;
}

bool UsdBridgeUsdWriter::CreateDirectories()
{
  bool valid = true;

  if(MpiBaseSessionDirectory.empty())
    valid = Connect->CreateFolder("", true, true);

  bool folderMayExist = !Settings.CreateNewSession;

  valid = valid && Connect->CreateFolder(SessionDirectory.c_str(), true, folderMayExist);

#ifdef VALUE_CLIP_RETIMING
  valid = valid && Connect->CreateFolder((SessionDirectory + constring::manifestFolder).c_str(), true, folderMayExist);
  valid = valid && Connect->CreateFolder((SessionDirectory + constring::primStageFolder).c_str(), true, folderMayExist);
#endif
#ifdef TIME_CLIP_STAGES
  valid = valid && Connect->CreateFolder((SessionDirectory + constring::clipFolder).c_str(), true, folderMayExist);
#endif
#ifdef CUSTOM_PBR_MDL
  if(Settings.EnableMdlShader)
  {
    valid = valid && Connect->CreateFolder((SessionDirectory + constring::mdlFolder).c_str(), true, folderMayExist);
  }
#endif
  valid = valid && Connect->CreateFolder((SessionDirectory + constring::imgFolder).c_str(), true, folderMayExist);
  valid = valid && Connect->CreateFolder((SessionDirectory + constring::volFolder).c_str(), true, folderMayExist);

  if (!valid)
  {
    UsdBridgeLogMacro(this->LogObject, UsdBridgeLogLevel::ERR, "Something went wrong in the filesystem creating the required output folders (permissions?).");
  }

  return valid;
}

#ifdef CUSTOM_PBR_MDL
namespace
{
  void WriteMdlFromStrings(const char* string0, const char* string1, const char* fileName, const UsdBridgeConnection* Connect)
  {
    size_t strLen0 = std::strlen(string0);
    size_t strLen1 = std::strlen(string1);
    size_t totalStrLen = strLen0 + strLen1;
    char* Mdl_Contents = new char[totalStrLen];
    std::memcpy(Mdl_Contents, string0, strLen0);
    std::memcpy(Mdl_Contents + strLen0, string1, strLen1);

    Connect->WriteFile(Mdl_Contents, totalStrLen, fileName, true, false);

    delete[] Mdl_Contents;
  }
}

bool UsdBridgeUsdWriter::CreateMdlFiles()
{
  {
    std::string relMdlPath = constring::mdlFolder + std::string(constring::opaqueMaterialFile);
    this->MdlOpaqueRelFilePath = SdfAssetPath(relMdlPath);
    std::string mdlFileName = SessionDirectory + relMdlPath;

    WriteMdlFromStrings(Mdl_PBRBase_string, Mdl_PBRBase_string_opaque, mdlFileName.c_str(), Connect.get());
  }

  {
    std::string relMdlPath = constring::mdlFolder + std::string(constring::transparentMaterialFile);
    this->MdlTranslucentRelFilePath = SdfAssetPath(relMdlPath);
    std::string mdlFileName = SessionDirectory + relMdlPath;

    WriteMdlFromStrings(Mdl_PBRBase_string, Mdl_PBRBase_string_translucent, mdlFileName.c_str(), Connect.get());
  }

  return true;
}
#endif

bool UsdBridgeUsdWriter::InitializeSession()
{
#ifdef OMNIVERSE_CONNECTION_ENABLE
  InitializeUsdPlugins(this->LogObject);
#endif

  if (ConnectionSettings.HostName.empty())
  {
    if(ConnectionSettings.WorkingDirectory.compare("void") == 0)
      Connect = std::make_unique<UsdBridgeVoidConnection>();
    else
      Connect = std::make_unique<UsdBridgeLocalConnection>();
  }
  else
    Connect = std::make_unique<UsdBridgeRemoteConnection>();

  Connect->Initialize(ConnectionSettings, this->LogObject);

  if(Settings.ParallelController)
  {
    int sessionNr = -1;
    if(Settings.ParallelController->GetRank() == 0)
      sessionNr = FindSessionNumber();

    Settings.ParallelController->BroadcastInt(sessionNr);
    SessionNumber = sessionNr;
  }
  else
  {
    SessionNumber = FindSessionNumber();
  }

  std::string sessionBase = constring::sessionPf + std::to_string(SessionNumber) + "/";

  if(Settings.MpiRank >= 0 && Settings.MpiSize > 0)
  {
    MpiBaseSessionDirectory = sessionBase;
    SessionDirectory = sessionBase + "rank_" + std::to_string(Settings.MpiRank) + "/";

    if(Settings.ParallelController->GetRank() == 0)
    {
      Connect->CreateFolder("", true, true);
      Connect->CreateFolder(MpiBaseSessionDirectory.c_str(), true, true);
    }
    Settings.ParallelController->Barrier();
  }
  else
  {
    SessionDirectory = sessionBase;
  }

  bool valid = true;

  if(this->EnableSaving)
  {
    valid = CreateDirectories();

#ifdef CUSTOM_PBR_MDL
    if(Settings.EnableMdlShader)
    {
      valid = valid && CreateMdlFiles();
    }
#endif
  }

  valid = valid && VolumeWriter->Initialize(this->LogObject);

  return valid;
}

void UsdBridgeUsdWriter::ResetSession()
{
  this->SessionNumber = -1;
  this->SceneStage = nullptr;
  this->MpiBaseSessionDirectory.clear();
}

void UsdBridgeUsdWriter::CreateParallelEncapsulatingFile()
{
  if(Settings.MpiRank != 0 || Settings.MpiSize <= 0)
    return;

  bool binary = Settings.BinaryOutput;
  const char* sceneFileName = binary ? constring::fullSceneNameBin : constring::fullSceneNameAscii;

  std::string encFileName = MpiBaseSessionDirectory + (binary ? constring::parallelSceneNameBin : constring::parallelSceneNameAscii);
  const char* absEncFilePath = Connect->GetUrl(encFileName.c_str());

  UsdStageRefPtr encStage = UsdStage::CreateNew(absEncFilePath);
  if(!encStage)
  {
    UsdBridgeLogMacro(this->LogObject, UsdBridgeLogLevel::ERR,
      "Failed to create parallel encapsulating USD file at " << absEncFilePath);
    return;
  }

  SdfPath rootPath("/Root");
  UsdPrim rootPrim = encStage->DefinePrim(rootPath);
  encStage->SetDefaultPrim(rootPrim);

  for(int rank = 0; rank < Settings.MpiSize; ++rank)
  {
    std::string rankRelPath = "./rank_" + std::to_string(rank) + "/" + sceneFileName;

    std::string rankPrimName = "rank_" + std::to_string(rank);
    SdfPath rankPrimPath = rootPath.AppendChild(TfToken(rankPrimName));

    UsdPrim rankPrim = encStage->DefinePrim(rankPrimPath);
    rankPrim.GetReferences().AddReference(rankRelPath, SdfPath("/Root"));
  }

  encStage->Save();

  UsdBridgeLogMacro(this->LogObject, UsdBridgeLogLevel::STATUS,
    "Created parallel encapsulating USD file at " << absEncFilePath
    << " referencing " << Settings.MpiSize << " ranks");
}

void UsdBridgeUsdWriter::CreateFrameEntryStage(
  const char* frameName,
  const SdfPath& worldPath,
  const SdfPath& cameraPath,
  const SdfPath& contextPath)
{
  if(!this->EnableSaving || !frameName || worldPath.IsEmpty() || cameraPath.IsEmpty() || contextPath.IsEmpty())
    return;

  if(!this->Connect || this->SessionDirectory.empty())
    return;

  bool binary = this->Settings.BinaryOutput;
  const char* sceneFileName = binary ? constring::fullSceneNameBin : constring::fullSceneNameAscii;
  const char* entryExt = binary ? ".usd" : ".usda";

  std::string entryRelPath = this->SessionDirectory + std::string(frameName) + entryExt;
  this->Connect->RemoveFile(entryRelPath.c_str(), true);

  const char* absEntryPath = this->Connect->GetUrl(entryRelPath.c_str());
  if(!absEntryPath)
  {
    UsdBridgeLogMacro(this->LogObject, UsdBridgeLogLevel::ERR,
      "Failed to resolve URL for frame entry stage '" << entryRelPath << "'");
    return;
  }
  UsdStageRefPtr entryStage = UsdStage::CreateNew(absEntryPath);
  if(!entryStage)
  {
    UsdBridgeLogMacro(this->LogObject, UsdBridgeLogLevel::ERR,
      "Failed to create frame entry USD file at " << absEntryPath);
    return;
  }

  std::string fullSceneRef = std::string("./") + sceneFileName;

  UsdPrim worldPrim = entryStage->DefinePrim(worldPath);
  worldPrim.GetReferences().AddReference(fullSceneRef, worldPath);

  UsdPrim cameraPrim = entryStage->DefinePrim(cameraPath);
  cameraPrim.GetReferences().AddReference(fullSceneRef, cameraPath);

  UsdPrim contextPrim = entryStage->DefinePrim(contextPath);
  contextPrim.GetReferences().AddReference(fullSceneRef, contextPath);

  SdfPath settingsPath = contextPath.AppendChild(UsdBridgeRenderTokens->Settings);
  SdfPath productPath = contextPath.AppendChild(UsdBridgeRenderTokens->Product);

  UsdRenderSettings settings = UsdRenderSettings(entryStage->OverridePrim(settingsPath));
  UsdRelationship settingsCamRel = settings.CreateCameraRel();
  settingsCamRel.SetTargets({cameraPath});

  UsdRenderProduct product = UsdRenderProduct(entryStage->OverridePrim(productPath));
  UsdRelationship productCamRel = product.CreateCameraRel();
  productCamRel.SetTargets({cameraPath});

  UsdPrim rootPrim = entryStage->GetPrimAtPath(
    SdfPath::AbsoluteRootPath().AppendChild(UsdBridgeTokens->Root));
  if(rootPrim)
    entryStage->SetDefaultPrim(rootPrim);

  entryStage->Save();

  UsdBridgeLogMacro(this->LogObject, UsdBridgeLogLevel::STATUS,
    "Created frame entry USD file at " << absEntryPath
    << " (world=" << worldPath << ", camera=" << cameraPath
    << ", context=" << contextPath << ")");
}

void UsdBridgeUsdWriter::RemoveFrameEntryStage(const char* frameName)
{
  if(!this->EnableSaving || !frameName || !this->Connect || this->SessionDirectory.empty())
    return;

  bool binary = this->Settings.BinaryOutput;
  const char* entryExt = binary ? ".usd" : ".usda";
  std::string entryRelPath = this->SessionDirectory + std::string(frameName) + entryExt;
  this->Connect->RemoveFile(entryRelPath.c_str(), true);
}

bool UsdBridgeUsdWriter::OpenSceneStage()
{
  bool binary = this->Settings.BinaryOutput;

  this->SceneFileName = this->SessionDirectory;
  this->SceneFileName += (binary ? constring::fullSceneNameBin : constring::fullSceneNameAscii);

#ifdef REPLACE_SCENE_BY_EXTERNAL_STAGE
  if (this->ExternalSceneStage)
  {
    this->SceneStage = this->ExternalSceneStage;
  }
#endif

  if (!this->SceneStage)
  {
    if(this->EnableSaving)
    {
      const char* absSceneFile = Connect->GetUrl(this->SceneFileName.c_str());
      if (!Settings.CreateNewSession)
        this->SceneStage = UsdStage::Open(absSceneFile);
      if (!this->SceneStage)
        this->SceneStage = UsdStage::CreateNew(absSceneFile);
    }
    else
    {
      this->SceneStage = UsdStage::CreateInMemory();
    }
  }

  if (!this->SceneStage)
  {
    UsdBridgeLogMacro(this->LogObject, UsdBridgeLogLevel::ERR, "Scene UsdStage cannot be created or opened. Maybe a filesystem issue?");
    return false;
  }
#ifndef REPLACE_SCENE_BY_EXTERNAL_STAGE
  else if (this->ExternalSceneStage)
  {
    ExternalSceneStage->GetRootLayer()->InsertSubLayerPath(
      this->SceneStage->GetRootLayer()->GetIdentifier());
  }
#endif

  this->RootClassName = constring::rootClassName;
  UsdPrim rootClassPrim = this->SceneStage->CreateClassPrim(SdfPath(this->RootClassName));
  assert(rootClassPrim);
  this->RootName = constring::rootPrimName;
  UsdPrim rootPrim = this->SceneStage->DefinePrim(SdfPath(this->RootName));
  assert(rootPrim);

#ifndef REPLACE_SCENE_BY_EXTERNAL_STAGE
  if (!this->ExternalSceneStage)
#endif
  {
    this->SceneStage->SetDefaultPrim(rootPrim);
    UsdModelAPI(rootPrim).SetKind(KindTokens->assembly);
  }

  if(!this->SceneStage->HasAuthoredTimeCodeRange())
  {
    this->SceneStage->SetStartTimeCode(StartTime);
    this->SceneStage->SetEndTimeCode(EndTime);
  }
  else
  {
    StartTime = this->SceneStage->GetStartTimeCode();
    EndTime = this->SceneStage->GetEndTimeCode();
  }

  if(this->EnableSaving)
    this->SceneStage->Save();
  else
    this->TrackStageMemory("FullScene", this->SceneStage);

  return true;
}

UsdStageRefPtr UsdBridgeUsdWriter::GetSceneStage() const
{
  return this->SceneStage;
}

UsdStageRefPtr UsdBridgeUsdWriter::GetTimeVarStage(UsdBridgePrimCache* cache
#ifdef TIME_CLIP_STAGES
  , bool useClipStage, const char* clipPf, double timeStep
  , std::function<void (UsdStageRefPtr)> initFunc
#endif
  ) const
{
#ifdef VALUE_CLIP_RETIMING
#ifdef TIME_CLIP_STAGES
  if (useClipStage)
  {
    bool exists;
    UsdStageRefPtr clipStage = this->FindOrCreateClipStage(cache, clipPf, timeStep, exists).second;

    if(!exists)
      initFunc(clipStage);

    return clipStage;
  }
  else
    return cache->GetPrimStagePair().second;
#else
  return cache->GetPrimStagePair().second;
#endif
#else
  return this->SceneStage;
#endif
}

#ifdef VALUE_CLIP_RETIMING
void UsdBridgeUsdWriter::CreateManifestStage(const char* name, const char* primPostfix, UsdBridgePrimCache* cacheEntry)
{
  bool binary = this->Settings.BinaryOutput;

  cacheEntry->ManifestStage.first = constring::manifestFolder + std::string(name) + primPostfix + (binary ? ".usd" : ".usda");

  if(this->EnableSaving)
  {
    std::string absoluteFileName = Connect->GetUrl((this->SessionDirectory + cacheEntry->ManifestStage.first).c_str());

    UsdBridgeDiagnosticMgrDelegate::SetOutputEnabled(false);
    cacheEntry->ManifestStage.second = UsdStage::CreateNew(absoluteFileName);
    UsdBridgeDiagnosticMgrDelegate::SetOutputEnabled(true);

    if (!cacheEntry->ManifestStage.second)
      cacheEntry->ManifestStage.second = UsdStage::Open(absoluteFileName);
  }
  else
  {
    cacheEntry->ManifestStage.second = UsdStage::CreateInMemory();
  }

  assert(cacheEntry->ManifestStage.second);

  cacheEntry->ManifestStage.second->DefinePrim(SdfPath(this->RootClassName));

  if(!this->EnableSaving)
    this->TrackStageMemory(std::string(name) + primPostfix + " (manifest)", cacheEntry->ManifestStage.second);
}

namespace {
struct RawGeoIdleState
{
  std::vector<uint8_t> baseline;
  uint64_t same = 0;
  bool burstDone = false;
};
std::map<std::string, RawGeoIdleState> g_rawGeoIdleStates;

struct RawGeoCurveIdleState
{
  uint64_t hash = 0;
  uint64_t same = 0;
};
std::map<std::string, RawGeoCurveIdleState> g_rawGeoCurveIdleStates;
} // namespace

void UsdBridgeUsdWriter::RemoveStoreEntriesForFile(const std::string& relativeName)
{
  if(relativeName.empty())
    return;

  // The curve writer stores `_s0.geo` ... `_s7.geo` siblings next to the base
  // `.geo`; a prim removal has to invalidate the whole family.
  std::vector<std::string> keys = jusync_rawgeo::StoreKeysForFile(relativeName);
  if(keys.size() == 1)
  {
    auto endsWith = [](const std::string& s, const char* suf) {
      size_t n = strlen(suf);
      return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
    };
    if(!endsWith(relativeName, ".png") && !endsWith(relativeName, ".exr") &&
       !endsWith(relativeName, ".tif") && !endsWith(relativeName, ".vdb"))
      keys.push_back(StoreKeyForStageName(relativeName)); // e.g. manifest names
  }

  for(const std::string& key : keys)
  {
    if(g_rankMemoryStore && g_rankMemoryStore->RemoveFile(key))
    {
      UsdBridgeLogMacro(this->LogObject, UsdBridgeLogLevel::STATUS,
        "[Purge] removed stale file from memory store: " << key);
    }
    g_rawGeoIdleStates.erase(key);
    g_rawGeoCurveIdleStates.erase(key);
    for(auto it = MemoryTracking.begin(); it != MemoryTracking.end(); ++it)
    {
      if(it->filename == key)
      {
        MemoryTracking.erase(it);
        break;
      }
    }
  }
}

void UsdBridgeUsdWriter::RemoveManifestAndClipStages(const UsdBridgePrimCache* cacheEntry)
{
  if(!this->EnableSaving)
  {
    if (!cacheEntry->ManifestStage.first.empty())
      RemoveStoreEntriesForFile(StoreKeyForStageName(cacheEntry->ManifestStage.first));

    for (auto& x : cacheEntry->ClipStages)
      RemoveStoreEntriesForFile(StoreKeyForStageName(x.second.first));

    return;
  }

  if(cacheEntry->ManifestStage.second)
  {
    cacheEntry->ManifestStage.second->RemovePrim(SdfPath(RootClassName));

    assert(!cacheEntry->ManifestStage.first.empty());
    Connect->RemoveFile((SessionDirectory + cacheEntry->ManifestStage.first).c_str(), true);
  }

  for (auto& x : cacheEntry->ClipStages)
  {
    Connect->RemoveFile((SessionDirectory + x.second.first).c_str(), true);
  }
}

const UsdStagePair& UsdBridgeUsdWriter::FindOrCreatePrimStage(UsdBridgePrimCache* cacheEntry, const char* namePostfix) const
{
  bool exists;
  return FindOrCreatePrimClipStage(cacheEntry, namePostfix, false, UsdBridgePrimCache::PrimStageTimeCode, exists);
}

const UsdStagePair& UsdBridgeUsdWriter::FindOrCreateClipStage(UsdBridgePrimCache* cacheEntry, const char* namePostfix, double timeStep, bool& exists) const
{
  return FindOrCreatePrimClipStage(cacheEntry, namePostfix, true, timeStep, exists);
}

const UsdStagePair& UsdBridgeUsdWriter::FindOrCreatePrimClipStage(UsdBridgePrimCache* cacheEntry, const char* namePostfix, bool isClip, double timeStep, bool& exists) const
{
  exists = true;
  bool binary = this->Settings.BinaryOutput;

  auto it = cacheEntry->ClipStages.find(timeStep);
  if (it == cacheEntry->ClipStages.end())
  {
    const char* folder = constring::primStageFolder;
    std::string fullNamePostfix(namePostfix);
    if(isClip)
    {
      folder = constring::clipFolder;

      // The rank in the key has to come from the same source as the "rank_<n>/"
      // session directories and ParallelScene references. Preferring the
      // communicator rank and falling back to the launcher environment keeps the
      // two in agreement; if they disagree (sub-communicator, oversubscription, or
      // a launcher exporting none of SLURM/OMPI/PMI), two ranks announce the same
      // clip and .geo name and the client alternates between their contents.
      int mpiRank = this->Settings.MpiRank;
      int mpiSize = this->Settings.MpiSize;
      if (mpiRank < 0 || mpiSize <= 0)
      {
        mpiRank = 0;
        mpiSize = 1;
        GetMpiRankSizeFromEnv(mpiRank, mpiSize);
      }

      if (mpiSize > 1) {
        fullNamePostfix += "_r" + std::to_string(mpiRank);
      }

      fullNamePostfix += "_" + std::to_string(timeStep);
    }
    std::string relativeFileName = folder + cacheEntry->Name.GetString() + fullNamePostfix + (binary ? ".usd" : ".usda");

    UsdStageRefPtr primClipStage;

    if(this->EnableSaving)
    {
      std::string absoluteFileName = Connect->GetUrl((this->SessionDirectory + relativeFileName).c_str());

      UsdBridgeDiagnosticMgrDelegate::SetOutputEnabled(false);
      primClipStage = UsdStage::CreateNew(absoluteFileName);
      UsdBridgeDiagnosticMgrDelegate::SetOutputEnabled(true);

      exists = !primClipStage;

      SdfPath rootPrimPath(this->RootClassName);
      if (exists)
      {
        primClipStage = UsdStage::Open(absoluteFileName); //Could happen if written folder is reused
        assert(primClipStage->GetPrimAtPath(rootPrimPath));
      }
      else
        primClipStage->DefinePrim(rootPrimPath);
    }
    else
    {
      primClipStage = UsdStage::CreateInMemory();
      exists = false;
      SdfPath rootPrimPath(this->RootClassName);
      primClipStage->DefinePrim(rootPrimPath);

      if(!this->EnableSaving)
      {
        std::string stageName = folder + cacheEntry->Name.GetString() + fullNamePostfix;
        const_cast<UsdBridgeUsdWriter*>(this)->TrackStageMemory(stageName, primClipStage);
      }
    }

    it = cacheEntry->ClipStages.emplace(timeStep, UsdStagePair(std::move(relativeFileName), primClipStage)).first;
  }
  return it->second;
}
#endif


void UsdBridgeUsdWriter::GetRootPrimPath(const SdfPath& name, const char* primPathCp, SdfPath& rootPrimPath)
{
  rootPrimPath = SdfPath(this->RootName);
  rootPrimPath = rootPrimPath.AppendPath(SdfPath(primPathCp));
  rootPrimPath = rootPrimPath.AppendPath(name);
}

void UsdBridgeUsdWriter::AddRootPrim(UsdBridgePrimCache* primCache, const char* primPathCp, const char* layerId)
{
  SdfPath primPath;
  GetRootPrimPath(primCache->Name, primPathCp, primPath);

  UsdPrim sceneGraphPrim = SceneStage->DefinePrim(primPath);
  assert(sceneGraphPrim);

  UsdReferences primRefs = sceneGraphPrim.GetReferences();
  primRefs.ClearReferences();
#ifdef VALUE_CLIP_RETIMING
  if(layerId)
  {
    primRefs.AddReference(layerId, primCache->PrimPath);
  }
#endif

  primRefs.AddInternalReference(primCache->PrimPath);
}

void UsdBridgeUsdWriter::RemoveRootPrim(UsdBridgePrimCache* primCache, const char* primPathCp)
{
  SdfPath primPath;
  GetRootPrimPath(primCache->Name, primPathCp, primPath);

  this->SceneStage->RemovePrim(primPath);
}

const std::string& UsdBridgeUsdWriter::CreatePrimName(const char * name, const char * category)
{
  (this->TempNameStr = this->RootClassName).append("/").append(category).append("/").append(name);
  return this->TempNameStr;
}

const std::string& UsdBridgeUsdWriter::GetResourceFileName(const std::string& basePath, double timeStep, const char* fileExtension)
{
  this->TempNameStr = basePath;
#ifdef TIME_BASED_CACHING
  this->TempNameStr += "_";
  this->TempNameStr += std::to_string(timeStep);
#endif
  this->TempNameStr += fileExtension;

  return this->TempNameStr;
}

const std::string& UsdBridgeUsdWriter::GetResourceFileName(const char* folderName, const std::string& objectName, double timeStep, const char* fileExtension)
{
  return GetResourceFileName(folderName + objectName, timeStep, fileExtension);
}

const std::string& UsdBridgeUsdWriter::GetResourceFileName(const char* folderName, const char* optionalObjectName, const std::string& defaultObjectName, double timeStep, const char* fileExtension)
{
  return GetResourceFileName(folderName, (optionalObjectName ? std::string(optionalObjectName) : defaultObjectName), timeStep, fileExtension);
}

bool UsdBridgeUsdWriter::CreatePrim(const SdfPath& path)
{
  UsdPrim classPrim = SceneStage->GetPrimAtPath(path);
  if(!classPrim)
  {
    classPrim = SceneStage->DefinePrim(path);
    assert(classPrim);
    return true;
  }
  return false;
}

void UsdBridgeUsdWriter::DeletePrim(const UsdBridgePrimCache* cacheEntry)
{
  if(SceneStage->GetPrimAtPath(cacheEntry->PrimPath))
    SceneStage->RemovePrim(cacheEntry->PrimPath);

#ifdef VALUE_CLIP_RETIMING
  RemoveManifestAndClipStages(cacheEntry);
#endif
}

#ifdef TIME_BASED_CACHING
void UsdBridgeUsdWriter::InitializePrimVisibility(UsdStageRefPtr stage, const SdfPath& primPath, const UsdTimeCode& timeCode,
  UsdBridgePrimCache* parentCache, UsdBridgePrimCache* childCache)
{
  UsdGeomImageable imageable = UsdGeomImageable::Get(stage, primPath);

  if (imageable)
  {
    UsdAttribute visAttrib = imageable.GetVisibilityAttr(); // in case MakeVisible fails
    if(!visAttrib)
      visAttrib = imageable.CreateVisibilityAttr(VtValue(UsdGeomTokens->invisible)); // default is invisible

    double startTime = stage->GetStartTimeCode();
    double endTime = stage->GetEndTimeCode();
    if (startTime < timeCode)
      visAttrib.Set(VtValue(UsdGeomTokens->invisible), startTime);//imageable.MakeInvisible(startTime);
    if (endTime > timeCode)
      visAttrib.Set(VtValue(UsdGeomTokens->invisible), endTime);//imageable.MakeInvisible(endTime);
    visAttrib.Set(VtValue(UsdGeomTokens->inherited), timeCode);//imageable.MakeVisible(timeCode);

    parentCache->SetChildVisibleAtTime(childCache, timeCode.GetValue());
  }
}

void UsdBridgeUsdWriter::SetPrimVisible(UsdStageRefPtr stage, const SdfPath& primPath, const UsdTimeCode& timeCode,
  UsdBridgePrimCache* parentCache, UsdBridgePrimCache* childCache)
{
  UsdGeomImageable imageable = UsdGeomImageable::Get(stage, primPath);
  if (imageable)
  {
    UsdAttribute visAttrib = imageable.GetVisibilityAttr();
    assert(visAttrib);

    visAttrib.Set(VtValue(UsdGeomTokens->inherited), timeCode);//imageable.MakeVisible(timeCode);
    parentCache->SetChildVisibleAtTime(childCache, timeCode.GetValue());
  }
}

void UsdBridgeUsdWriter::PrimRemoveIfInvisibleAnytime(UsdStageRefPtr stage, const UsdPrim& prim, bool timeVarying, const UsdTimeCode& timeCode, AtRemoveRefFunc atRemoveRef,
  UsdBridgePrimCache* parentCache, UsdBridgePrimCache* primCache)
{
  const SdfPath& primPath = prim.GetPath();

  bool removePrim = true; // TimeVarying is not automatically removed

  if (timeVarying)
  {
    if(primCache)
    {
      // (so if the timecode wasn't found in the cache, do not remove the prim)

      removePrim = parentCache->SetChildInvisibleAtTime(primCache, timeCode.GetValue());

      if(!removePrim)
      {
        UsdGeomImageable imageable = UsdGeomImageable::Get(stage, primPath);
        if (imageable)
        {
          UsdAttribute visAttrib = imageable.GetVisibilityAttr();
          if (visAttrib)
            visAttrib.Set(UsdGeomTokens->invisible, timeCode);
        }
      }
    }
    else
      removePrim = false;
  }

  if (removePrim)
  {
    if(primCache)
      atRemoveRef(parentCache, primCache);

    stage->RemovePrim(primPath);
  }
}

void UsdBridgeUsdWriter::ChildrenRemoveIfInvisibleAnytime(UsdStageRefPtr stage, UsdBridgePrimCache* parentCache, const SdfPath& parentPath, bool timeVarying, const UsdTimeCode& timeCode, AtRemoveRefFunc atRemoveRef, const SdfPath& exceptPath)
{
  UsdPrim parentPrim = stage->GetPrimAtPath(parentPath);
  if (parentPrim)
  {
    UsdPrimSiblingRange children = parentPrim.GetAllChildren();
    for (UsdPrim child : children)
    {
      UsdBridgePrimCache* childCache = parentCache->GetChildCache(child.GetName());

      if (child.GetPath() != exceptPath)
        PrimRemoveIfInvisibleAnytime(stage, child, timeVarying, timeCode, atRemoveRef,
          parentCache, childCache);
    }
  }
}

#endif


#ifdef VALUE_CLIP_RETIMING
void UsdBridgeUsdWriter::InitializeClipMetaData(const UsdPrim& clipPrim, UsdBridgePrimCache* childCache, double parentTimeStep, double childTimeStep, bool clipStages, const char* clipPostfix)
{
  UsdClipsAPI clipsApi(clipPrim);

  clipsApi.SetClipPrimPath(childCache->PrimPath.GetString());

  const std::string& manifestPath = childCache->ManifestStage.first;
  const std::string* refStagePath;
#ifdef TIME_CLIP_STAGES
  if (clipStages)
  {

    bool exists;
    const UsdStagePair& childStagePair = FindOrCreateClipStage(childCache, clipPostfix, childTimeStep, exists);
    if(!exists)
      UsdBridgeLogMacro(this->LogObject, UsdBridgeLogLevel::WARNING, "Child clip stage not found while setting clip metadata, using generated stage instead. Probably the child data has not been properly updated.");

    refStagePath = &childStagePair.first;
  }
  else
#endif
  {
    refStagePath = &childCache->GetPrimStagePair().first;
  }

  clipsApi.SetClipManifestAssetPath(SdfAssetPath(manifestPath));

  VtArray<SdfAssetPath> assetPaths;
  assetPaths.push_back(SdfAssetPath(*refStagePath));
  clipsApi.SetClipAssetPaths(assetPaths);

  VtVec2dArray clipActives;
  clipActives.push_back(GfVec2d(parentTimeStep, 0));
  clipsApi.SetClipActive(clipActives);

  VtVec2dArray clipTimes;
  clipTimes.push_back(GfVec2d(parentTimeStep, childTimeStep));
  clipsApi.SetClipTimes(clipTimes);
}

void UsdBridgeUsdWriter::UpdateClipMetaData(const UsdPrim& clipPrim, UsdBridgePrimCache* childCache, double parentTimeStep, double childTimeStep, bool clipStages, const char* clipPostfix)
{
  UsdClipsAPI clipsApi(clipPrim);

#ifdef TIME_CLIP_STAGES
  if (clipStages)
  {
    bool exists;
    const UsdStagePair& childStagePair = FindOrCreateClipStage(childCache, clipPostfix, childTimeStep, exists);

    const std::string& refStagePath = childStagePair.first;

    VtVec2dArray clipActives;
    clipsApi.GetClipActive(&clipActives);
    VtArray<SdfAssetPath> assetPaths;
    clipsApi.GetClipAssetPaths(&assetPaths);

    SdfAssetPath refStageSdf(refStagePath);
    auto assetIt = std::find_if(assetPaths.begin(), assetPaths.end(), [&refStageSdf](const SdfAssetPath& entry) -> bool { return refStageSdf.GetAssetPath() == entry.GetAssetPath(); });
    int assetIndex = int(assetIt - assetPaths.begin());
    bool newAsset = (assetIndex == assetPaths.size());

    {
      int timeFindIdx = 0;
      for (; timeFindIdx < clipActives.size() && clipActives[timeFindIdx][0] != parentTimeStep; ++timeFindIdx)
      {}

      bool replaceAsset = false;

      if (timeFindIdx == clipActives.size())
        clipActives.push_back(GfVec2d(parentTimeStep, assetIndex));
      else
      {
        double prevAssetIndex = clipActives[timeFindIdx][1];

        if (newAsset)
        {
          int assetIdxFindIdx = 0;
          for (; assetIdxFindIdx < clipActives.size() &&
            (assetIdxFindIdx == timeFindIdx || clipActives[assetIdxFindIdx][1] != prevAssetIndex);
            ++assetIdxFindIdx)
          {}

          replaceAsset = (assetIdxFindIdx == clipActives.size());
        }

        if(replaceAsset)
          assetPaths[int(prevAssetIndex)] = refStageSdf;
        else
          clipActives[timeFindIdx][1] = assetIndex;
      }

      if (newAsset && !replaceAsset)
        assetPaths.push_back(refStageSdf);

      clipsApi.SetClipAssetPaths(assetPaths);
      clipsApi.SetClipActive(clipActives);
    }
  }
#endif

  VtVec2dArray clipTimes;
  clipsApi.GetClipTimes(&clipTimes);
  {
    int findIdx = 0;
    for (; findIdx < clipTimes.size(); ++findIdx)
    {
      if (clipTimes[findIdx][0] == parentTimeStep)
      {
        clipTimes[findIdx][1] = childTimeStep;
        break;
      }
    }
    if (findIdx == clipTimes.size())
      clipTimes.push_back(GfVec2d(parentTimeStep, childTimeStep));
    clipsApi.SetClipTimes(clipTimes);
  }
}

#endif

SdfPath UsdBridgeUsdWriter::AddRef_NoClip(UsdBridgePrimCache* parentCache, UsdBridgePrimCache* childCache, const char* refPathExt,
  bool timeVarying, double parentTimeStep, bool instanceable,
  const RefModFuncs& refModCallbacks)
{
  return AddRef_Impl(parentCache, childCache, refPathExt, timeVarying, false, false, nullptr, parentTimeStep, parentTimeStep, instanceable, refModCallbacks);
}

SdfPath UsdBridgeUsdWriter::AddRef(UsdBridgePrimCache* parentCache, UsdBridgePrimCache* childCache, const char* refPathExt,
  bool timeVarying, bool valueClip, bool clipStages, const char* clipPostfix,
  double parentTimeStep, double childTimeStep, bool instanceable,
  const RefModFuncs& refModCallbacks)
{
  return AddRef_Impl(parentCache, childCache, refPathExt, timeVarying, valueClip, clipStages, clipPostfix, parentTimeStep, childTimeStep, instanceable, refModCallbacks);
}

SdfPath UsdBridgeUsdWriter::AddRef_Impl(UsdBridgePrimCache* parentCache, UsdBridgePrimCache* childCache, const char* refPathExt,
  bool timeVarying,
  bool valueClip,   // Retiming through a value clip
  bool clipStages,
  const char* clipPostfix, double parentTimeStep, double childTimeStep, bool instanceable,
  const RefModFuncs& refModCallbacks)
{
  UsdTimeCode parentTimeCode(parentTimeStep);

  SdfPath childBasePath = parentCache->PrimPath;
  if (refPathExt)
    childBasePath = parentCache->PrimPath.AppendPath(SdfPath(refPathExt));

  SdfPath referencingPrimPath = childBasePath.AppendPath(childCache->Name);
  UsdPrim referencingPrim = SceneStage->GetPrimAtPath(referencingPrimPath);

  if (!referencingPrim)
  {
    referencingPrim = SceneStage->DefinePrim(referencingPrimPath);
    assert(referencingPrim);

#ifdef VALUE_CLIP_RETIMING
    if (valueClip)
      InitializeClipMetaData(referencingPrim, childCache, parentTimeStep, childTimeStep, clipStages, clipPostfix);
#endif

    {
      UsdReferences references = referencingPrim.GetReferences(); //references or inherits?
      references.ClearReferences();
      references.AddInternalReference(childCache->PrimPath);
#ifdef VALUE_CLIP_RETIMING
      if(!valueClip)
      {
        auto it = childCache->ClipStages.find(UsdBridgePrimCache::PrimStageTimeCode);
        if(it != childCache->ClipStages.end())
          references.AddReference(it->second.first, childCache->PrimPath);
      }
#endif
    }

    refModCallbacks.AtNewRef(parentCache, childCache);

#ifdef TIME_BASED_CACHING
    if (timeVarying)
      InitializePrimVisibility(SceneStage, referencingPrimPath, parentTimeCode,
        parentCache, childCache);
#endif
  }
  else
  {
#ifdef TIME_BASED_CACHING
    if (timeVarying)
      SetPrimVisible(SceneStage, referencingPrimPath, parentTimeCode,
        parentCache, childCache);

#ifdef VALUE_CLIP_RETIMING
    if (valueClip)
      UpdateClipMetaData(referencingPrim, childCache, parentTimeStep, childTimeStep, clipStages, clipPostfix);
#endif
#endif
  }

  if(instanceable || referencingPrim.HasAuthoredInstanceable())
    referencingPrim.SetInstanceable(instanceable);

  return referencingPrimPath;
}

void UsdBridgeUsdWriter::RemoveAllRefs(UsdBridgePrimCache* parentCache, const char* refPathExt, bool timeVarying, double timeStep, AtRemoveRefFunc atRemoveRef)
{
  SdfPath childBasePath = parentCache->PrimPath;
  if (refPathExt)
    childBasePath = parentCache->PrimPath.AppendPath(SdfPath(refPathExt));

  RemoveAllRefs(SceneStage, parentCache, childBasePath, timeVarying, timeStep, atRemoveRef);
}

void UsdBridgeUsdWriter::RemoveAllRefs(UsdStageRefPtr stage, UsdBridgePrimCache* parentCache, SdfPath childBasePath, bool timeVarying, double timeStep, AtRemoveRefFunc atRemoveRef)
{
  UsdPrim parentPrim = stage->GetPrimAtPath(childBasePath);
  if(parentPrim)
  {
    size_t removedCount = 0;
    UsdPrimSiblingRange children = parentPrim.GetAllChildren();
    for (UsdPrim child : children)
    {
      UsdBridgePrimCache* childCache = parentCache->GetChildCache(child.GetName());
      std::string childName = child.GetName().GetString();

#ifdef TIME_BASED_CACHING
      UsdTimeCode timeCode(timeStep);
      bool wasRemoved = true; // assume removed, check PrimRemoveIfInvisibleAnytime
      PrimRemoveIfInvisibleAnytime(stage, child, timeVarying, timeCode, atRemoveRef,
        parentCache, childCache);
      UsdBridgeLogMacro(this->LogObject, UsdBridgeLogLevel::WARNING,
        "USDRMNG: RemoveAllRefs — removed prim '" << childName
        << "' from '" << parentCache->PrimPath.GetNameToken().GetString()
        << "' at time " << timeStep
        );
      removedCount++;
#else
      if(childCache)
      {
        atRemoveRef(parentCache, childCache); // Decrease reference count in caches
        UsdBridgeLogMacro(this->LogObject, UsdBridgeLogLevel::WARNING,
          "USDRMNG: RemoveAllRefs — removed prim '" << childName
          << "' from '" << parentCache->PrimPath.GetNameToken().GetString()
          << "' at time " << timeStep
          );
      }
      stage->RemovePrim(child.GetPath()); // Remove reference prim
      removedCount++;
#endif
    }

    if (removedCount > 0)
    {
      UsdBridgeLogMacro(this->LogObject, UsdBridgeLogLevel::STATUS,
        "USDRMNG: RemoveAllRefs summary — removed " << removedCount << " prims from '"
        << parentCache->PrimPath.GetNameToken().GetString() << "' at time " << timeStep
        );
    }
  }
}

void UsdBridgeUsdWriter::ManageUnusedRefs(UsdBridgePrimCache* parentCache, const UsdBridgePrimCacheList& newChildren, const char* refPathExt, bool timeVarying, double timeStep, AtRemoveRefFunc atRemoveRef)
{
  ManageUnusedRefs(SceneStage, parentCache, newChildren, refPathExt, timeVarying, timeStep, atRemoveRef);
}

void UsdBridgeUsdWriter::ManageUnusedRefs(UsdStageRefPtr stage, UsdBridgePrimCache* parentCache, const UsdBridgePrimCacheList& newChildren, const char* refPathExt, bool timeVarying, double timeStep, AtRemoveRefFunc atRemoveRef)
{
  UsdTimeCode timeCode(timeStep);

  UsdPrim basePrim;
  if (refPathExt)
  {
    SdfPath childBasePath = parentCache->PrimPath.AppendPath(SdfPath(refPathExt));
    basePrim = stage->GetPrimAtPath(childBasePath);
  }
  else
    basePrim = stage->GetPrimAtPath(parentCache->PrimPath);

  if (basePrim)
  {
    UsdPrimSiblingRange children = basePrim.GetAllChildren();

    std::set<std::string> newChildNames;
    for (size_t i = 0; i < newChildren.size(); ++i)
      newChildNames.insert(newChildren[i]->PrimPath.GetNameToken().GetString());

    size_t removedCount = 0;
    for (UsdPrim oldChild : children)
    {
      std::string oldName = oldChild.GetName().GetString();
      bool found = newChildNames.count(oldName) > 0;

      UsdBridgePrimCache* oldChildCache = parentCache->GetChildCache(oldChild.GetName());

      if (!found)
      {
        const char* extStr = refPathExt ? refPathExt : "(root)";
        UsdBridgeLogMacro(this->LogObject, UsdBridgeLogLevel::WARNING,
          "USDRMNG: ManageUnusedRefs — removing orphaned prim '" << oldName
          << "' from parent '" << parentCache->PrimPath.GetNameToken().GetString()
          << "." << extStr << "' at time " << timeStep
          << " (newChildren count: " << newChildren.size() << ")"
          );
#ifdef TIME_BASED_CACHING
        {
          if(timeVarying)
          {
            PrimRemoveIfInvisibleAnytime(stage, oldChild, timeVarying, timeCode, atRemoveRef,
              parentCache, oldChildCache);
            if(!stage->GetPrimAtPath(oldChild.GetPath()))
              removedCount++;
          }
          else
          {
            if(oldChildCache)
              atRemoveRef(parentCache, oldChildCache);
            stage->RemovePrim(oldChild.GetPath());
            removedCount++;
          }
        }
#else
        {// remove the whole referencing prim
          if(oldChildCache)
            atRemoveRef(parentCache, oldChildCache);
          stage->RemovePrim(oldChild.GetPath());
          removedCount++;
        }
#endif
      }
    }

    if (removedCount > 0)
    {
      UsdBridgeLogMacro(this->LogObject, UsdBridgeLogLevel::STATUS,
        "USDRMNG: ManageUnusedRefs summary — removed " << removedCount << " orphaned prims from '"
        << parentCache->PrimPath.GetNameToken().GetString() << "' at time " << timeStep << ", newChildren=" << newChildren.size()
        );
    }
  }
}

void UsdBridgeUsdWriter::InitializeUsdTransform(const UsdBridgePrimCache* cacheEntry)
{
  SdfPath transformPath = cacheEntry->PrimPath;
  UsdGeomXform transform = GetOrDefinePrim<UsdGeomXform>(SceneStage, transformPath);
  assert(transform);
}

void UsdBridgeUsdWriter::InitializeUsdCamera(UsdStageRefPtr cameraStage, const SdfPath& cameraPath)
{
  UsdGeomCamera cameraPrim = GetOrDefinePrim<UsdGeomCamera>(cameraStage, cameraPath);
  assert(cameraPrim);

  cameraPrim.CreateProjectionAttr();
  cameraPrim.CreateHorizontalApertureAttr();
  cameraPrim.CreateVerticalApertureAttr();
  cameraPrim.CreateFocalLengthAttr();
  cameraPrim.CreateClippingRangeAttr();
  cameraPrim.CreateFocusDistanceAttr();
}

void UsdBridgeUsdWriter::BindMaterialToGeom(const SdfPath & refGeomPath, const SdfPath & refMatPath)
{
  UsdPrim refGeomPrim = this->SceneStage->GetPrimAtPath(refGeomPath);
  assert(refGeomPrim);

  UsdShadeMaterial refMatPrim = UsdShadeMaterial::Get(this->SceneStage, refMatPath);
  assert(refMatPrim);

  UsdShadeMaterialBindingAPI(refGeomPrim).Bind(refMatPrim);
}

void UsdBridgeUsdWriter::UnbindMaterialFromGeom(const SdfPath & refGeomPath)
{
  UsdPrim refGeomPrim = this->SceneStage->GetPrimAtPath(refGeomPath);
  assert(refGeomPrim);

  UsdShadeMaterialBindingAPI(refGeomPrim).UnbindDirectBinding();
}

void UsdBridgeUsdWriter::UpdateUsdTransform(const SdfPath& transPrimPath, const float* transform, bool timeVarying, double timeStep)
{
  TimeEvaluator<bool> timeEval(timeVarying, timeStep);

  GfMatrix4d transMat;
  transMat.SetRow(0, GfVec4d(GfVec4f(&transform[0])));
  transMat.SetRow(1, GfVec4d(GfVec4f(&transform[4])));
  transMat.SetRow(2, GfVec4d(GfVec4f(&transform[8])));
  transMat.SetRow(3, GfVec4d(GfVec4f(&transform[12])));

  UsdGeomXform tfPrim = UsdGeomXform::Get(this->SceneStage, transPrimPath);
  assert(tfPrim);
  tfPrim.ClearXformOpOrder();
  UsdGeomXformOp xformOp = tfPrim.AddTransformOp();
  ClearAndSetUsdAttribute(xformOp.GetAttr(), transMat, timeEval.Eval(), !timeEval.TimeVarying);
}

void UsdBridgeUsdWriter::UpdateUsdCamera(UsdStageRefPtr timeVarStage, const SdfPath& cameraPrimPath,
  const UsdBridgeCameraData& cameraData, double timeStep, bool timeVarHasChanged)
{
  const TimeEvaluator<UsdBridgeCameraData> timeEval(cameraData, timeStep);
  typedef UsdBridgeCameraData::DataMemberId DMI;

  UsdGeomCamera cameraPrim = UsdGeomCamera::Get(timeVarStage, cameraPrimPath);
  assert(cameraPrim);

  GfVec3d position(cameraData.Position.Data);
  GfVec3d fwdDir(cameraData.Direction.Data);
  GfVec3d worldY(cameraData.Up.Data);
  GfVec3d worldZ(-fwdDir);

  GfVec3d worldX = GfCross(worldY, worldZ);
  worldX.Normalize();
  worldY = GfCross(worldZ, worldX);
  worldY.Normalize();

  GfMatrix4d camMatrix(
    worldX[0], worldX[1], worldX[2], 0.0,
    worldY[0], worldY[1], worldY[2], 0.0,
    worldZ[0], worldZ[1], worldZ[2], 0.0,
    position[0], position[1], position[2], 1.0
  );

  cameraPrim.ClearXformOpOrder();
  UsdGeomXformOp xformOp = cameraPrim.AddTransformOp();
  ClearAndSetUsdAttribute(xformOp.GetAttr(), camMatrix, timeEval.Eval(DMI::VIEW),
    timeVarHasChanged && !timeEval.IsTimeVarying(DMI::VIEW));

  const GfVec3f camScaling(0.01f); // Add a default cam scaling for UI purposes
  UsdGeomXformOp scaleOp = cameraPrim.AddScaleOp();
  scaleOp.GetAttr().Set(camScaling, UsdTimeCode::Default());

  GfCamera gfCam;
  gfCam.SetPerspectiveFromAspectRatioAndFieldOfView(cameraData.Aspect, GfRadiansToDegrees(cameraData.Fovy), GfCamera::FOVVertical);

  UsdTimeCode projectTime = timeEval.Eval(DMI::PROJECTION);
  bool clearProjAttrib = timeVarHasChanged && !timeEval.IsTimeVarying(DMI::PROJECTION);

  ClearAndSetUsdAttribute(cameraPrim.GetProjectionAttr(),
    (gfCam.GetProjection() == GfCamera::Perspective) ? UsdGeomTokens->perspective : UsdGeomTokens->orthographic,
    projectTime, clearProjAttrib);
  ClearAndSetUsdAttribute(cameraPrim.GetHorizontalApertureAttr(), gfCam.GetHorizontalAperture(), projectTime, clearProjAttrib);
  ClearAndSetUsdAttribute(cameraPrim.GetVerticalApertureAttr(), gfCam.GetVerticalAperture(), projectTime, clearProjAttrib);
  ClearAndSetUsdAttribute(cameraPrim.GetFocalLengthAttr(), gfCam.GetFocalLength(), projectTime, clearProjAttrib);
  ClearAndSetUsdAttribute(cameraPrim.GetClippingRangeAttr(), GfVec2f(cameraData.Near, cameraData.Far), projectTime, clearProjAttrib);
}

void UsdBridgeUsdWriter::UpdateBeginEndTime(double timeStep)
{
  if (timeStep < StartTime)
  {
    StartTime = timeStep;
    SceneStage->SetStartTimeCode(timeStep);
  }
  if (timeStep > EndTime)
  {
    EndTime = timeStep;
    SceneStage->SetEndTimeCode(timeStep);
  }
}

TfToken& UsdBridgeUsdWriter::AttributeNameToken(const char* attribName)
{
  int i = 0;
  for(; i < AttributeTokens.size(); ++i)
  {
    if(AttributeTokens[i] == attribName) // Overloaded == operator on TfToken
      break;
  }
  if(i == AttributeTokens.size())
    AttributeTokens.emplace_back(TfToken(attribName));
  return AttributeTokens[i];
}

void UsdBridgeUsdWriter::AddSharedResourceRef(const UsdBridgeResourceKey& key)
{
  bool found = false;
  for(auto& entry : SharedResourceCache)
  {
    if(entry.first == key)
    {
      ++entry.second.first;
      found = true;
    }
  }
  if(!found)
    SharedResourceCache.push_back(SharedResourceKV(key, SharedResourceValue(1, false)));
}

bool UsdBridgeUsdWriter::RemoveSharedResourceRef(const UsdBridgeResourceKey& key)
{
  bool removed = false;
  SharedResourceContainer::iterator it = SharedResourceCache.begin();
  while(it != SharedResourceCache.end())
  {
    if(it->first == key)
      --it->second.first;

    if(it->second.first == 0)
    {
      it = SharedResourceCache.erase(it);
      removed = true;
    }
    else
      ++it;
  }
  return removed;
}

bool UsdBridgeUsdWriter::SetSharedResourceModified(const UsdBridgeResourceKey& key)
{
  bool modified = false;
  for(auto& entry : SharedResourceCache)
  {
    if(entry.first == key)
    {
      modified = entry.second.second;
      entry.second.second = true;
    }
  }
  return modified;
}

void UsdBridgeUsdWriter::ResetSharedResourceModified()
{
  for(auto& entry : SharedResourceCache)
  {
    entry.second.second = false;
  }
}

void RemoveResourceFiles(UsdBridgePrimCache* cache, UsdBridgeUsdWriter& usdWriter,
  const char* resourceFolder, const char* fileExtension)
{

  assert(cache->ResourceKeys);
  if(!cache || !cache->ResourceKeys)
    return;
  UsdBridgePrimCache::ResourceContainer& keys = *(cache->ResourceKeys);

  std::string basePath = usdWriter.SessionDirectory; basePath.append(resourceFolder);
  for (const UsdBridgeResourceKey& key : keys)
  {
    bool removeFile = true;
    if(key.name)
      removeFile = usdWriter.RemoveSharedResourceRef(key);

    if(removeFile)
    {
      double timeStep =
#ifdef TIME_BASED_CACHING
        key.timeStep;
#else
        0.0;
#endif
      const std::string& resFileName = usdWriter.GetResourceFileName(basePath.c_str(), key.name, cache->Name.GetString(), timeStep, fileExtension);
      usdWriter.Connect->RemoveFile(resFileName.c_str(), true);

      std::string rel = resFileName;
      const std::string& dir = usdWriter.SessionDirectory;
      if(!dir.empty() && rel.size() > dir.size() && rel.compare(0, dir.size(), dir) == 0)
        rel.erase(0, dir.size());
      usdWriter.RemoveStoreEntriesForFile(rel);
    }
  }
  keys.resize(0);
}

void UsdBridgeUsdWriter::ExportLayerToString(const SdfLayerRefPtr& rootLayer,
                                             const std::string& stageName,
                                             std::string& outContent,
                                             bool& outIsBinary)
{
  outContent.clear();
  outIsBinary = false;
  if(!rootLayer)
    return;

  if(Settings.BinaryOutput)
  {
#ifdef _WIN32
    const char* defaultCrateTmp = "";   // no portable RAM-backed fs on Windows
#else
    const char* defaultCrateTmp = "/dev/shm";
#endif
    const char* envTmp = getenv("JUSYNC_CRATE_TMP");
    const std::string crateDir(envTmp ? envTmp : defaultCrateTmp);
    if(!crateDir.empty())
    {
      // RecalculateAllMemoryUsage (broker thread) can export concurrently.

      static std::atomic<uint64_t> tmpCounter{0};
      const std::string tmpPath = crateDir + "/jusync_crate_" +
                                  std::to_string(tmpCounter.fetch_add(1)) + ".usdc";

      bool exported = false;
      if(rootLayer->Export(tmpPath))
      {
        std::ifstream in(tmpPath, std::ios::binary);
        if(in)
        {
          outContent.assign((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
          in.close();
          exported = !outContent.empty();
        }
      }
      std::error_code ec;
      std::filesystem::remove(tmpPath, ec);

      if(exported && outContent.size() >= 8 &&
         std::memcmp(outContent.data(), "PXR-USDC", 8) == 0)
      {
        outIsBinary = true;
        return;
      }

      std::cerr << "[ExportLayerToString] WARNING: crate export for '" << stageName
                << "' did not produce PXR-USDC bytes (dir '" << crateDir
                << "'), falling back to ASCII" << std::endl;
      outContent.clear();
    }
  }

  rootLayer->ExportToString(&outContent);
}

std::string UsdBridgeUsdWriter::StoreKeyForStageName(const std::string& stageName) const
{
  std::string filename;
  if(stageName == "FullScene")
    filename = "FullScene.usda";
  else
  {
    filename = stageName;
    if (filename.length() >= 4)
    {
      std::string ext4 = filename.substr(filename.length() - 4);
      std::string ext5 = filename.substr(filename.length() - 5);
      if (ext4 != ".usd" && ext4 != ".usda" && ext5 != ".usda")
        filename += ".usda";
    }
    else
      filename += ".usda";
  }
  return filename;
}

void UsdBridgeUsdWriter::TrackStageMemory(const std::string& stageName, UsdStageRefPtr stage)
{
  if(!stage)
    return;

  // ExportLayerToString honors Settings.BinaryOutput (usd::serialize.outputBinary):

  // (zstd magic, then crate binary vs "#usda").

  size_t estimatedBytes = 0;
  std::string fullUsdContent;
  bool isBinaryContent = false;

  auto rootLayer = stage->GetRootLayer();
  if(rootLayer)
  {
    ExportLayerToString(rootLayer, stageName, fullUsdContent, isBinaryContent);
    estimatedBytes = fullUsdContent.size();
  }

  std::string filename;
  if(stageName == "FullScene")
    filename = "FullScene.usda";
  else
  {
    filename = stageName;
    if (filename.length() >= 4)
    {
      std::string ext4 = filename.substr(filename.length() - 4);
      std::string ext5 = filename.substr(filename.length() - 5);
      if (ext4 != ".usd" && ext4 != ".usda" && ext5 != ".usda")
        filename += ".usda";
    }
    else
      filename += ".usda";
  }

  const uint64_t contentHash = fullUsdContent.empty()
      ? 0 : XXH64(fullUsdContent.data(), fullUsdContent.size(), 0);

  if(g_rankMemoryStore != nullptr && !fullUsdContent.empty())
  {
    GetDiffCapture().CapturePreStore(filename);

    g_rankMemoryStore->StoreFile(
      filename,
      fullUsdContent.data(),
      fullUsdContent.size(),
      isBinaryContent ? "model/vnd.usd-crate" : "text/plain"
    );

    std::cout << "[TrackStageMemory] Stored '" << filename << "': " << fullUsdContent.size()
              << " bytes (" << (isBinaryContent ? "binary .usdc" : "ascii .usda") << ")" << std::endl;
  }

  for (auto it = MemoryTracking.begin(); it != MemoryTracking.end(); ++it) {
    if (it->filename == filename) {
      it->stage = stage;
      it->estimatedBytes = estimatedBytes;
      it->lastExportedHash = contentHash;
      goto tracking_done;
    }
  }
  {
    StageMemoryInfo info;
    info.name = stageName;
    info.filename = filename;
    info.estimatedBytes = estimatedBytes;
    info.stage = stage; // Store reference for later recalculation
    info.lastExportedHash = contentHash;
    MemoryTracking.push_back(info);
  }
tracking_done:;

  double sizeMB = estimatedBytes / (1024.0 * 1024.0);
  UsdBridgeLogMacro(this->LogObject, UsdBridgeLogLevel::STATUS,
    "In-memory stage '" << stageName << "': ~" << sizeMB << " MB");
}

size_t UsdBridgeUsdWriter::GetTotalMemoryUsage() const
{
  size_t total = 0;
  for(const auto& info : MemoryTracking)
  {
    total += info.estimatedBytes;
  }
  return total;
}

void UsdBridgeUsdWriter::RecalculateAllMemoryUsage()
{
  std::cout << "[RecalculateAllMemoryUsage] Processing " << MemoryTracking.size() << " tracked stages" << std::endl;

  for(auto& info : MemoryTracking)
  {
    if(info.stage)
    {
      size_t estimatedBytes = 0;
      std::string fullUsdContent;
      bool isBinaryContent = false;

      auto rootLayer = info.stage->GetRootLayer();
      if(rootLayer)
      {
        ExportLayerToString(rootLayer, info.filename.empty() ? info.name : info.filename,
                            fullUsdContent, isBinaryContent);
        estimatedBytes = fullUsdContent.size();
      }

       info.estimatedBytes = estimatedBytes;

       if(g_rankMemoryStore != nullptr)
       {
          if (fullUsdContent.empty() && info.stage)
          {
            std::cout << "[RecalculateAllMemoryUsage] WARNING: empty export for '"
                      << (info.filename.empty() ? info.name : info.filename)
                      << "' — keeping previous entry" << std::endl;
          }
          else if (!fullUsdContent.empty())
          {
            const uint64_t contentHash = XXH64(fullUsdContent.data(), fullUsdContent.size(), 0);
            if (info.lastExportedHash == contentHash)
            {
              info.estimatedBytes = fullUsdContent.size();
              continue;
            }
            info.lastExportedHash = contentHash;

            const std::string& filename = info.filename.empty() ? info.name : info.filename;

            g_rankMemoryStore->StoreFile(
              filename,
              fullUsdContent.data(),
              fullUsdContent.size(),
              isBinaryContent ? "model/vnd.usd-crate" : "text/plain"
            );

            std::cout << "[RecalculateAllMemoryUsage] Updated '" << filename << "': " << fullUsdContent.size()
                      << " bytes (" << (isBinaryContent ? "binary .usdc" : "ascii .usda") << ")" << std::endl;
          }
       }
    }
  }
}


namespace
{
  constexpr uint64_t kMaxRawGeoPayloadBytes = 1536ull * 1024 * 1024;

  uint32_t RawGeoFloatComponents(UsdBridgeType type)
  {
    switch(type)
    {
      case UsdBridgeType::FLOAT:  return 1;
      case UsdBridgeType::FLOAT2: return 2;
      case UsdBridgeType::FLOAT3: return 3;
      case UsdBridgeType::FLOAT4: return 4;
      default:                    return 0;
    }
  }

  bool RawGeoEndsWith(const std::string& s, const char* suf)
  {
    size_t n = strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
  }

  // Shared between the point (v2) and curve (v3) writers: turn the clip-stage
  // name for this timestep into the store key, without the USD extension.
  bool RawGeoGeoKey(UsdBridgePrimCache* cacheEntry, double timeStep, std::string& outGeoKey)
  {
    if(!cacheEntry)
      return false;

    auto clipIt = cacheEntry->ClipStages.find(timeStep);
    if(clipIt == cacheEntry->ClipStages.end())
      return false;

    outGeoKey = clipIt->second.first;
    if(RawGeoEndsWith(outGeoKey, ".usda"))
      outGeoKey.resize(outGeoKey.size() - 5);
    else if(RawGeoEndsWith(outGeoKey, ".usd"))
      outGeoKey.resize(outGeoKey.size() - 4);
    outGeoKey += ".geo";
    return true;
  }

  bool RawGeoCurveEnabled()
  {
    static const bool enabled = [](){
      const char* v = getenv("JUSYNC_RAWGEO_CURVES");
      return v && v[0] == '1';
    }();
    return enabled;
  }

  uint64_t RawGeoCurveSubsets()
  {
    static const uint64_t subsets = [](){
      const char* v = getenv("JUSYNC_RAWGEO_CURVE_SUBSETS");
      uint64_t s = (v && v[0]) ? strtoull(v, nullptr, 10) : 4;
      if(s < 1) s = 1;
      if(s > 8) s = 8;
      return s;
    }();
    return subsets;
  }

  uint64_t RawGeoHash(const void* data, size_t bytes, uint64_t seed)
  {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for(size_t i = 0; i < bytes; ++i)
    {
      h ^= static_cast<uint64_t>(p[i]);
      h *= 1099511628211ull;
    }
    return h;
  }
}

bool UsdBridgeUsdWriter::RawGeoEnabled()
{
  static const bool enabled = [](){
    const char* v = getenv("JUSYNC_RAWGEO");
    return v && v[0] == '1';
  }();
  return enabled;
}

bool UsdBridgeUsdWriter::TryStoreRawGeo(UsdBridgePrimCache* cacheEntry,
    const UsdBridgeInstancerData& data, double timeStep)
{
  if(!RawGeoEnabled() || !g_rankMemoryStore || !cacheEntry)
    return false;

  // RawGeo v2 has no topology or prototype shape. Only sphere point clouds
  // may use it; surface instancers fall through to the USD point-instancer
  // writer so cylinders/cones/glyphs keep their geometry.
  if(!UsdBridgeCanStreamPointCloud(data))
    return false;

  std::string geoKey;
  if(!RawGeoGeoKey(cacheEntry, timeStep, geoKey))
    return false;

  const uint64_t nSrc = data.NumPoints;

  static const uint64_t stride = [](){
    const char* v = getenv("JUSYNC_RAWGEO_STRIDE");
    uint64_t s = (v && v[0]) ? strtoull(v, nullptr, 10) : 1;
    if(s < 1) s = 1;
    if(s > 1024) s = 1024;
    return s;
  }();
  static const bool strideLogged = [](){
    if(stride > 1)
      fprintf(stderr, "[UsdBridge] RawGeo stride = %llu\n", (unsigned long long)stride);
    return true;
  }();

  uint32_t colorComponents = data.Colors ? RawGeoFloatComponents(data.ColorsType) : 0;
  if(colorComponents != 3 && colorComponents != 4)
    colorComponents = 0;

  constexpr uint32_t kSyntheticAttr = 0xFFFFFFFFu;
  anari_usd_protocol::JusyncRawGeoAttrDesc attrDescs[16];
  uint32_t attrSrcIdx[16];
  uint32_t numAttrs = 0;
  bool hasParticleIdAttribute = false;
  if(data.Attributes)
  {
    for(uint32_t a = 0; a < data.NumAttributes && numAttrs < 16; ++a)
    {
      const UsdBridgeAttribute& src = data.Attributes[a];
      if(!src.Data || src.PerPrimData)
        continue;
      const uint32_t comps = RawGeoFloatComponents(src.DataType);
      if(comps == 0)
        continue;
      if(src.Name && strcmp(src.Name, "particle_id") == 0)
        hasParticleIdAttribute = true;
      memset(&attrDescs[numAttrs], 0, sizeof(attrDescs[numAttrs]));
      if(src.Name)
        strncpy(attrDescs[numAttrs].name, src.Name, sizeof(attrDescs[numAttrs].name) - 1);
      attrDescs[numAttrs].components = comps;
      attrSrcIdx[numAttrs] = a;
      ++numAttrs;
    }
  }

  // Point streams carry a stable particle ID even though the existing
  // instancer path does not expose one as a generic attribute. The wire stays
  // v2: the ID is exact 16-bit chunks in floats, counted against the existing
  // 16-attribute and payload limits. Build it once at full resolution so the
  // idle full-density burst reuses the same data.
  std::vector<float> syntheticIds;
  if(!hasParticleIdAttribute && data.InstanceIds && numAttrs < 16)
  {
    const auto idEncoding = anari_usd_rawgeo::GetParticleIdEncoding(data.InstanceIdsType);
    if(idEncoding.Components != 0 &&
       anari_usd_rawgeo::PackParticleIds(data.InstanceIds, data.InstanceIdsType, nSrc, 1, syntheticIds))
    {
      memset(&attrDescs[numAttrs], 0, sizeof(attrDescs[numAttrs]));
      strncpy(attrDescs[numAttrs].name, "particle_id", sizeof(attrDescs[numAttrs].name) - 1);
      attrDescs[numAttrs].components = idEncoding.Components;
      attrSrcIdx[numAttrs] = kSyntheticAttr;
      ++numAttrs;
    }
  }

  const size_t descBytes = numAttrs * sizeof(anari_usd_protocol::JusyncRawGeoAttrDesc);
  auto packRawGeo = [&](uint64_t effStride, anari_usd_protocol::JusyncRawGeoHeaderV2 &outHdr)
  {
    const uint64_t n = (effStride == 1) ? nSrc : ((nSrc + effStride - 1) / effStride);

    memset(&outHdr, 0, sizeof(outHdr));
    outHdr.magic = anari_usd_protocol::JUSYNC_RAWGEO_MAGIC;
    outHdr.version = 2; // point clouds stay on the v2 wire format; v3 is curves only
    strncpy(outHdr.prim_name, cacheEntry->Name.GetString().c_str(),
        sizeof(outHdr.prim_name) - 1);
    outHdr.num_points = n;
    outHdr.time = timeStep;
    outHdr.position_components = 3;
    outHdr.color_components = colorComponents;
    outHdr.num_attributes = numAttrs;
    outHdr.attr_uniform_count = static_cast<uint32_t>(n);
    // v2 requires a well-formed lattice even when the point path emits one frame.
    outHdr.subset_index = 0;
    outHdr.subset_count = 1;
    outHdr.full_num_points = n;

    const uint64_t posBytes = n * 3 * sizeof(float);
    const uint64_t colBytes = colorComponents ? n * colorComponents * sizeof(float) : 0;
    uint64_t attrBytes = 0;
    for(uint32_t a = 0; a < numAttrs; ++a)
      attrBytes += n * attrDescs[a].components * sizeof(float);
    outHdr.payload_bytes = posBytes + colBytes + attrBytes;

    std::vector<uint8_t> b;
    b.resize(sizeof(outHdr) + descBytes + outHdr.payload_bytes);
    uint8_t* d = b.data();
    memcpy(d, &outHdr, sizeof(outHdr));
    d += sizeof(outHdr);
    if(numAttrs)
    {
      memcpy(d, attrDescs, descBytes);
      d += descBytes;
    }
    if(effStride == 1)
    {
      memcpy(d, data.Points, posBytes);
    }
    else
    {
      const float* src = static_cast<const float*>(data.Points);
      for(uint64_t i = 0; i < n; ++i)
        memcpy(d + i * 3 * sizeof(float), src + (i * effStride) * 3, 3 * sizeof(float));
    }
    d += posBytes;
    if(colBytes)
    {
      if(effStride == 1)
      {
        memcpy(d, data.Colors, colBytes);
      }
      else
      {
        const float* src = static_cast<const float*>(data.Colors);
        const uint64_t cc = colorComponents;
        for(uint64_t i = 0; i < n; ++i)
          memcpy(d + i * cc * sizeof(float), src + (i * effStride) * cc, cc * sizeof(float));
      }
      d += colBytes;
    }
    for(uint32_t a = 0; a < numAttrs; ++a)
    {
      const uint64_t comps = attrDescs[a].components;
      const uint64_t bytes = n * comps * sizeof(float);
      const float* attrData = (attrSrcIdx[a] == kSyntheticAttr)
          ? syntheticIds.data()
          : static_cast<const float*>(data.Attributes[attrSrcIdx[a]].Data);
      if(effStride == 1)
      {
        memcpy(d, attrData, bytes);
      }
      else
      {
        for(uint64_t i = 0; i < n; ++i)
          memcpy(d + i * comps * sizeof(float), attrData + (i * effStride) * comps, comps * sizeof(float));
      }
      d += bytes;
    }
    return b;
  };

  anari_usd_protocol::JusyncRawGeoHeaderV2 hdr;
  std::vector<uint8_t> buf = packRawGeo(stride, hdr);

  if(hdr.payload_bytes > kMaxRawGeoPayloadBytes)
  {
    static const bool cappedLogged = [](){
      fprintf(stderr, "[UsdBridge] RawGeo payload exceeds cap, falling back to USD path\n");
      return true;
    }();
    (void)cappedLogged;
    return false;
  }

  static const uint64_t idleFrames = [](){
    const char* v = getenv("JUSYNC_RAWGEO_IDLE_FRAMES");
    uint64_t n = (v && v[0]) ? strtoull(v, nullptr, 10) : 6;
    return n > 1000000 ? 1000000 : n;
  }();
  if(idleFrames > 0)
  {
    RawGeoIdleState &st = g_rawGeoIdleStates[geoKey];
    const uint8_t* payload = buf.data() + sizeof(hdr) + descBytes;
    const bool identical = st.baseline.size() == hdr.payload_bytes
        && memcmp(payload, st.baseline.data(), hdr.payload_bytes) == 0;
    st.baseline.assign(payload, payload + hdr.payload_bytes);
    st.same = identical ? st.same + 1 : 0;
    if(!identical)
      st.burstDone = false;
    if(st.same >= idleFrames)
    {
      if(!st.burstDone && stride > 1)
      {
        const uint64_t perPoint = hdr.payload_bytes / hdr.num_points;
        st.burstDone = true;
        if(nSrc * perPoint <= kMaxRawGeoPayloadBytes)
        {
          buf = packRawGeo(1, hdr);
          fprintf(stderr, "[UsdBridge] RawGeo idle burst: full-res %llu points (%.1f MB) for '%s'\n",
              (unsigned long long)hdr.num_points, buf.size() / (1024.0 * 1024.0), geoKey.c_str());
        }
      }
      else
        return true; // quiet + already full density: no store, no notification
    }
  }

  static const char* snapDir = getenv("JUSYNC_RAWGEO_SNAPSHOT_DIR");
  static const uint64_t snapEvery = [](){
    const char* v = getenv("JUSYNC_RAWGEO_SNAPSHOT_EVERY");
    uint64_t e = (v && v[0]) ? strtoull(v, nullptr, 10) : 10;
    return e ? e : 10;
  }();
  static std::atomic<uint64_t> snapFrames{0};
  if(snapDir && snapDir[0])
  {
    const uint64_t f = snapFrames.fetch_add(1);
    if(f % snapEvery == 0)
    {
      const uint64_t n = hdr.num_points;
      const uint64_t posBytes = n * 3 * sizeof(float);
      const uint8_t* pos = buf.data() + sizeof(hdr) + descBytes;
      const uint8_t* col = pos + posBytes;
      char path[512];
      snprintf(path, sizeof(path), "%s/cloud_%08llu.ply", snapDir, (unsigned long long)f);
      FILE* ply = fopen(path, "wb");
      if(ply)
      {
        fprintf(ply,
            "ply\nformat binary_little_endian 1.0\nelement vertex %llu\n"
            "property float x\nproperty float y\nproperty float z\n",
            (unsigned long long)n);
        if(colorComponents == 4)
          fprintf(ply,
              "property float red\nproperty float green\nproperty float blue\n"
              "property float alpha\n");
        else if(colorComponents)
          fprintf(ply,
              "property float red\nproperty float green\nproperty float blue\n");
        fprintf(ply, "end_header\n");
        fwrite(pos, posBytes, 1, ply);
        if(colorComponents)
          fwrite(col, n * colorComponents * sizeof(float), 1, ply);
        fclose(ply);
      }
    }
  }

  GetDiffCapture().CapturePreStore(geoKey);
  g_rankMemoryStore->StoreFile(geoKey, buf.data(), buf.size(), "application/x-jusync-geo");
  return true;
}

bool UsdBridgeUsdWriter::TryStoreRawGeo(UsdBridgePrimCache* cacheEntry,
    const UsdBridgeCurveData& data, double timeStep)
{
  if(!RawGeoEnabled() || !RawGeoCurveEnabled() || !g_rankMemoryStore || !cacheEntry)
    return false;

  if(!data.Points || data.NumPoints == 0 ||
     RawGeoFloatComponents(data.PointsType) != 3 ||
     !data.CurveLengths || data.NumCurveLengths == 0)
    return false;

  std::string baseKey;
  if(!RawGeoGeoKey(cacheEntry, timeStep, baseKey))
    return false;

  const uint64_t numCurves = data.NumCurveLengths;

  std::vector<int32_t> lengths(numCurves);
  std::vector<uint64_t> offsets(numCurves + 1, 0);
  uint64_t summedPoints = 0;
  for(uint64_t c = 0; c < numCurves; ++c)
  {
    const int len = data.CurveLengths[c];
    if(len < 2)
    {
      static const bool logged = [](){
        fprintf(stderr, "[UsdBridge] RawGeo curves rejected: curve shorter than 2 nodes\n");
        return true;
      }();
      (void)logged;
      return false;
    }
    lengths[c] = static_cast<int32_t>(len);
    summedPoints += static_cast<uint64_t>(len);
    offsets[c + 1] = summedPoints;
  }
  if(summedPoints != data.NumPoints)
  {
    static const bool logged = [](){
      fprintf(stderr, "[UsdBridge] RawGeo curves rejected: CurveLengths sum does not match NumPoints\n");
      return true;
    }();
    (void)logged;
    return false;
  }

  const uint32_t rawColorComponents = (data.Colors && !data.PerPrimColors)
    ? RawGeoFloatComponents(data.ColorsType) : 0;
  const uint32_t colorComponents =
    (rawColorComponents == 3 || rawColorComponents == 4) ? rawColorComponents : 0;

  // Widths are per point only. A uniform scale is left to the client's
  // jusync.Curve.DefaultRadius rather than expanded into a per-point array.
  const bool hasWidths = data.Scales && data.ScalesType == UsdBridgeType::FLOAT;

  struct CurveAttrSrc
  {
    const char* Name = nullptr;
    uint32_t Components = 0;
    const void* Data = nullptr;
  };
  std::vector<CurveAttrSrc> attrs;
  if(data.Attributes)
  {
    for(uint32_t a = 0; a < data.NumAttributes && attrs.size() < 16; ++a)
    {
      const UsdBridgeAttribute& src = data.Attributes[a];
      if(!src.Data || src.PerPrimData || !src.Name)
        continue;
      const uint32_t comps = RawGeoFloatComponents(src.DataType);
      if(comps == 0 || comps > 4)
        continue;
      attrs.push_back(CurveAttrSrc{src.Name, comps, src.Data});
    }
  }

  const uint64_t subsetCount = RawGeoCurveSubsets();
  const std::string primName = cacheEntry->Name.GetString();

  // Build every subset before storing any: the cap must be able to reject the
  // whole frame without leaving a partial set of files behind.
  std::vector<std::vector<uint8_t>> payloads(subsetCount);

  for(uint64_t k = 0; k < subsetCount; ++k)
  {
    std::vector<int32_t> selectedLengths;
    uint64_t selectedPoints = 0;
    for(uint64_t c = k; c < numCurves; c += subsetCount)
    {
      selectedLengths.push_back(lengths[c]);
      selectedPoints += static_cast<uint64_t>(lengths[c]);
    }

    std::vector<float> selPoints(static_cast<size_t>(selectedPoints) * 3);
    std::vector<float> selColors(static_cast<size_t>(selectedPoints) * colorComponents);
    std::vector<float> selWidths(hasWidths ? static_cast<size_t>(selectedPoints) : 0);
    std::vector<std::vector<float>> selAttrs(attrs.size());
    for(size_t a = 0; a < attrs.size(); ++a)
      selAttrs[a].resize(static_cast<size_t>(selectedPoints) * attrs[a].Components);

    uint64_t out = 0;
    for(uint64_t c = k; c < numCurves; c += subsetCount)
    {
      const uint64_t start = offsets[c];
      const uint64_t len = static_cast<uint64_t>(lengths[c]);

      memcpy(selPoints.data() + out * 3,
          static_cast<const float*>(data.Points) + start * 3,
          static_cast<size_t>(len) * 3 * sizeof(float));

      if(colorComponents)
        memcpy(selColors.data() + out * colorComponents,
            static_cast<const float*>(data.Colors) + start * colorComponents,
            static_cast<size_t>(len) * colorComponents * sizeof(float));

      if(hasWidths)
        memcpy(selWidths.data() + out,
            static_cast<const float*>(data.Scales) + start,
            static_cast<size_t>(len) * sizeof(float));

      for(size_t a = 0; a < attrs.size(); ++a)
        memcpy(selAttrs[a].data() + out * attrs[a].Components,
            static_cast<const float*>(attrs[a].Data) + start * attrs[a].Components,
            static_cast<size_t>(len) * attrs[a].Components * sizeof(float));

      out += len;
    }

    std::vector<jusync_rawgeo::CurveAttribute> attrInputs(attrs.size());
    for(size_t a = 0; a < attrs.size(); ++a)
    {
      attrInputs[a].Name = attrs[a].Name;
      attrInputs[a].Components = attrs[a].Components;
      attrInputs[a].Data = selAttrs[a].empty() ? nullptr : selAttrs[a].data();
    }

    jusync_rawgeo::CurvePackInput in{};
    in.PrimName = primName.c_str();
    in.Time = timeStep;
    in.Points = selectedPoints ? selPoints.data() : nullptr;
    in.NumPoints = selectedPoints;
    in.Colors = (selectedPoints && colorComponents) ? selColors.data() : nullptr;
    in.ColorComponents = colorComponents;
    // An empty subset still advertises the frame's width component so the
    // client's merge flags do not depend on which subset happens to arrive
    // first. The pointer is never dereferenced when NumPoints is zero.
    static const float EmptyWidth = 0.0f;
    in.Widths = hasWidths ? (selectedPoints ? selWidths.data() : &EmptyWidth) : nullptr;
    in.CurveLengths = selectedLengths.empty() ? nullptr : selectedLengths.data();
    in.NumCurves = selectedLengths.size();
    in.Attributes = attrInputs.empty() ? nullptr : attrInputs.data();
    in.NumAttributes = static_cast<uint32_t>(attrInputs.size());
    in.SubsetIndex = k;
    in.SubsetCount = subsetCount;
    in.FullCurveCount = numCurves;
    in.FullNumPoints = data.NumPoints;

    if(!jusync_rawgeo::PackCurvePayload(in, payloads[k]))
    {
      static const bool logged = [](){
        fprintf(stderr, "[UsdBridge] RawGeo curve payload rejected, falling back to USD path\n");
        return true;
      }();
      (void)logged;
      return false;
    }
  }

  const size_t descBytes = attrs.size() * sizeof(anari_usd_protocol::JusyncRawGeoAttrDesc);
  for(size_t k = 0; k < payloads.size(); ++k)
  {
    if(payloads[k].size() >
       sizeof(anari_usd_protocol::JusyncRawGeoHeader) + descBytes + kMaxRawGeoPayloadBytes)
    {
      static const bool logged = [](){
        fprintf(stderr, "[UsdBridge] RawGeo curve payload exceeds cap, falling back to USD path\n");
        return true;
      }();
      (void)logged;
      return false;
    }
  }

  static const uint64_t idleFrames = [](){
    const char* v = getenv("JUSYNC_RAWGEO_IDLE_FRAMES");
    uint64_t n = (v && v[0]) ? strtoull(v, nullptr, 10) : 6;
    return n > 1000000 ? 1000000 : n;
  }();
  if(idleFrames > 0)
  {
    uint64_t h = 1469598103934665603ull;
    h = RawGeoHash(&data.NumPoints, sizeof(data.NumPoints), h);
    h = RawGeoHash(data.CurveLengths, static_cast<size_t>(numCurves) * sizeof(int), h);
    h = RawGeoHash(data.Points, static_cast<size_t>(data.NumPoints) * 3 * sizeof(float), h);
    if(colorComponents)
      h = RawGeoHash(data.Colors,
          static_cast<size_t>(data.NumPoints) * colorComponents * sizeof(float), h);
    if(hasWidths)
      h = RawGeoHash(data.Scales,
          static_cast<size_t>(data.NumPoints) * sizeof(float), h);

    RawGeoCurveIdleState& st = g_rawGeoCurveIdleStates[baseKey];
    st.same = (st.hash == h) ? st.same + 1 : 0;
    st.hash = h;
    if(st.same >= idleFrames)
      return true; // identical frame: leave the previous subset files in place
  }

  for(uint64_t k = 0; k < subsetCount; ++k)
  {
    const std::string key = jusync_rawgeo::AddSubsetSuffix(baseKey, k, subsetCount);
    GetDiffCapture().CapturePreStore(key);
    g_rankMemoryStore->StoreFile(key, payloads[k].data(), payloads[k].size(),
        "application/x-jusync-geo");
  }

  return true;
}