// Copyright 2020 The Khronos Group

#ifndef UsdBridgeRenderContext_h
#define UsdBridgeRenderContext_h

#include "usd.h"
#include "UsdBridgeData.h"

#include <memory>

class UsdBridgeUsdWriter;

#ifdef USD_DEVICE_RENDERING_ENABLED
PXR_NAMESPACE_OPEN_SCOPE
class HdRenderIndex;
class HdEngine;
class Hgi;
class UsdImagingDelegate;
PXR_NAMESPACE_CLOSE_SCOPE
#endif

enum class RenderContextMode
{
    Shared,     // Use shared core (efficient)
    Standalone  // Debug fallback (each context has own engine)
};

class UsdBridgeRenderContext
{
public:
    virtual ~UsdBridgeRenderContext() = default;

    virtual void SetCameraPath(const pxr::SdfPath& cameraPath) = 0;
    virtual void SetWorldPath(const pxr::SdfPath& worldPath) = 0;
    virtual void SetRenderBufferSize(uint32_t width, uint32_t height) = 0;
    virtual void Render(double timeStep) = 0;
    virtual bool FrameReady(bool wait) = 0;
    virtual void* MapFrame(UsdBridgeType& returnFormat) = 0;
    virtual void UnmapFrame() = 0;

    virtual bool IsInitialized() const = 0;
};

class UsdBridgeRendererCore
{
public:
    UsdBridgeRendererCore(UsdBridgeUsdWriter& usdWriter);
    ~UsdBridgeRendererCore();

    void Initialize(const char* rendererPluginName);

    bool IsInitialized() const;

#ifdef USD_DEVICE_RENDERING_ENABLED
    pxr::HdRenderIndex* GetRenderIndex();
    pxr::HdEngine* GetEngine();
    pxr::Hgi* GetHgi();
    pxr::UsdImagingDelegate* GetSceneDelegate();
#endif

    pxr::UsdStageRefPtr GetStage();
    UsdBridgeUsdWriter& GetUsdWriter();

protected:
    class Internals;
    std::unique_ptr<Internals> Data;
    UsdBridgeUsdWriter& UsdWriter;
};

std::unique_ptr<UsdBridgeRenderContext> CreateRenderContext(
    RenderContextMode mode,
    UsdBridgeRendererCore* core,  // Required for Shared mode, can be null for Standalone
    UsdBridgeUsdWriter& usdWriter,
    const char* rendererPluginName,
    const pxr::SdfPath& contextId);

#endif
