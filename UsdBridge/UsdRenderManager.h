// Copyright 2020 The Khronos Group

#ifndef UsdRenderManager_h
#define UsdRenderManager_h

#include "UsdBridgeData.h"
#include "UsdBridgeRenderSettingsState.h"
#include "usd.h"
#include <memory>
#include <unordered_map>
#include <string>

class UsdBridgeUsdWriter;
class UsdBridgeRendererCore;
class UsdBridgeRenderContext;

class UsdRenderManager
{
public:
    UsdRenderManager(UsdBridgeUsdWriter& usdWriter);
    ~UsdRenderManager();

    void RegisterFrame(const char* frameName);
    void UnregisterFrame(const char* frameName);
    void UnregisterFrameByState(void* frameState); // For unregistering when name changed
    void DeleteFrame(const char* frameName);

    void SetFrameRenderer(const char* frameName, const char* hydraRendererName);
    void SetFrameCamera(const char* frameName, const pxr::SdfPath& cameraPath);
    void SetFrameWorld(const char* frameName, const pxr::SdfPath& worldPath);
    void SetFrameRenderSize(const char* frameName, uint32_t width, uint32_t height);

    void Render(const char* frameName, double timeStep);
    bool FrameReady(const char* frameName, bool wait);
    void* MapFrame(const char* frameName, UsdBridgeType& returnFormat);
    void UnmapFrame(const char* frameName);

    bool IsRenderingEnabled() const;

    void* GetFrameState(const char* frameName);

private:
    UsdBridgeRendererCore* GetOrCreateCore(const char* hydraRendererName);

    struct FrameState;
    FrameState* GetFrameStateInternal(const char* frameName);

    void CreateFrameEntryStage(const char* frameName, FrameState* state);

    struct FrameState
    {
        std::string RendererName;
        pxr::SdfPath CameraPath;
        pxr::SdfPath WorldPath;
        UsdBridgeRenderSettingsState UsdRenderState;
        UsdBridgeRendererCore* Core = nullptr;
        std::unique_ptr<UsdBridgeRenderContext> Context;
    };

    UsdBridgeUsdWriter& UsdWriter;

    std::unordered_map<std::string, std::unique_ptr<UsdBridgeRendererCore>> Cores;

    std::unordered_map<std::string, FrameState> Frames;

    static constexpr const char* DefaultRendererName = "HdStormRendererPlugin";
};

#endif
