// Copyright 2020 The Khronos Group

#include "UsdFrame.h"
#include "UsdBridge/UsdBridge.h"
#include "UsdAnari.h"
#include "UsdDevice.h"
#include "UsdCamera.h"
#include "UsdRenderer.h"
#include "UsdWorld.h"
#include "anari/frontend/type_utility.h"

#define CameraType ANARI_CAMERA
#define RendererType ANARI_RENDERER
#define WorldType ANARI_WORLD
using CameraUsdType = AnariToUsdBridgedObject<CameraType>::Type;
using WorldUsdType = AnariToUsdBridgedObject<WorldType>::Type;

DEFINE_PARAMETER_MAP(UsdFrame,
  REGISTER_PARAMETER_MACRO("name", ANARI_STRING, name)
  REGISTER_PARAMETER_MACRO("usd::name", ANARI_STRING, usdName)
  REGISTER_PARAMETER_MACRO("size", ANARI_UINT32_VEC2, size)
  REGISTER_PARAMETER_MACRO("channel.color", ANARI_DATA_TYPE, color)
  REGISTER_PARAMETER_MACRO("channel.depth", ANARI_DATA_TYPE, depth)
  REGISTER_PARAMETER_MACRO("camera", CameraType, camera)
  REGISTER_PARAMETER_MACRO("renderer", RendererType, renderer)
  REGISTER_PARAMETER_MACRO("world", WorldType, world)
  REGISTER_PARAMETER_MACRO("usd::time", ANARI_FLOAT64, time)
)

UsdFrame::UsdFrame(const char* name, UsdDevice* device)
  : BridgedBaseObjectType(ANARI_FRAME, name, device)
{
  frameBridge = device->getUsdBridge();
}

UsdFrame::~UsdFrame()
{
  if (registeredFrameState)
  {
    frameBridge->UnregisterFrame(getName());
  }
  delete[] mappedColorMem;
  delete[] mappedDepthMem;
}

void UsdFrame::remove(UsdDevice* device)
{
  if (registeredFrameState)
  {
    if (!frameBridge)
      frameBridge = device->getUsdBridge();
    if (frameBridge)
      frameBridge->DeleteFrame(getName());
    registeredFrameState = nullptr;
  }
}

bool UsdFrame::deferCommit(UsdDevice* device)
{
  return false;
}

bool UsdFrame::doCommitData(UsdDevice* device)
{
  if (!frameBridge)
    frameBridge = device->getUsdBridge();
  if (!frameBridge)
    return false;

  const char* frameName = getName();
  void* currentFrameState = frameBridge->GetFrameState(frameName);

  if (registeredFrameState == nullptr || registeredFrameState != currentFrameState)
  {
    if (registeredFrameState)
    {
      frameBridge->UnregisterFrameByState(registeredFrameState);
    }

    frameBridge->RegisterFrame(frameName);
    registeredFrameState = frameBridge->GetFrameState(frameName);
  }

  const UsdFrameData& paramData = getReadParams();
  if (paramData.size.Data[0] > 0 && paramData.size.Data[1] > 0)
  {
    frameBridge->SetFrameRenderSize(frameName, paramData.size.Data[0], paramData.size.Data[1]);
  }

  return false;
}

const void* UsdFrame::mapBuffer(
  const char *channel,
  uint32_t *width,
  uint32_t *height,
  ANARIDataType *pixelType,
  UsdDevice* device)
{
  *width = renderBufferSize.Data[0];
  *height = renderBufferSize.Data[1];
  *pixelType = ANARI_UNKNOWN;

  if (strEquals(channel, "channel.color"))
  {
    if (registeredFrameState)
    {
      UsdBridgeType usdBridgeFormat;
      if(void* deviceBuffer = frameBridge->MapFrame(getName(), usdBridgeFormat))
      {
        ANARIDataType nativeType = UsdBridgeToAnariType(usdBridgeFormat);
        *pixelType = (anari::sizeOf(renderBufferColorFormat) == anari::sizeOf(nativeType))
          ? renderBufferColorFormat : nativeType;
        return deviceBuffer;
      }
    }

    mappedColorMem = ReserveBuffer(renderBufferColorFormat);
    *pixelType = renderBufferColorFormat;
    return mappedColorMem;
  }
  else if (strEquals(channel, "channel.depth"))
  {
    mappedDepthMem = ReserveBuffer(renderBufferDepthFormat);
    *pixelType = renderBufferDepthFormat;
    return mappedDepthMem;
  }

  *width = 0;
  *height = 0;

  return nullptr;
}

void UsdFrame::unmapBuffer(const char *channel, UsdDevice* device)
{
  if (registeredFrameState)
  {
    frameBridge->UnmapFrame(getName());
  }

  if (strEquals(channel, "channel.color"))
  {
    delete[] mappedColorMem;
    mappedColorMem = nullptr;
  }
  else if (strEquals(channel, "channel.depth"))
  {
    delete[] mappedDepthMem;
    mappedDepthMem = nullptr;
  }
}

char* UsdFrame::ReserveBuffer(ANARIDataType format)
{
  size_t formatSize = anari::sizeOf(format);
  size_t memSize = formatSize * renderBufferSize.Data[0] * renderBufferSize.Data[1];
  return new char[memSize];
}

void UsdFrame::saveUsd(UsdDevice* device)
{
  if (!frameBridge)
    frameBridge = device->getUsdBridge();
  if (frameBridge)
  {
    frameBridge->SaveScene();
  }
}

void UsdFrame::renderFrame(UsdDevice* device)
{
  const UsdFrameData& paramData = getReadParams();

  if (!frameBridge)
    frameBridge = device->getUsdBridge();
  if (!frameBridge || !registeredFrameState)
    return;

  const char* frameName = getName();

  if (paramData.world && !UsdObjectNotInitialized<WorldUsdType>(paramData.world))
  {
    UsdWorldHandle worldHandle = paramData.world->getUsdHandle();
    frameBridge->SetFrameWorld(frameName, worldHandle);
  }

  if (paramData.camera && !UsdObjectNotInitialized<CameraUsdType>(paramData.camera))
  {
    UsdCameraHandle cameraHandle = paramData.camera->getUsdHandle();
    frameBridge->SetFrameCamera(frameName, cameraHandle);
  }

  renderBufferSize = paramData.size;
  renderBufferColorFormat = paramData.color;
  renderBufferDepthFormat = paramData.depth;

  if (paramData.renderer && paramData.renderer->isHydraRenderer())
  {
    const char* hydraRendererName = paramData.renderer->getHydraRendererName();
    frameBridge->SetFrameRenderer(frameName, hydraRendererName);
    frameBridge->RenderFrame(frameName, paramData.time);
  }
}

bool UsdFrame::frameReady(ANARIWaitMask mask, UsdDevice* device)
{
  if (!registeredFrameState)
    return true;

  return frameBridge->FrameReady(getName(), mask == ANARI_WAIT);
}
