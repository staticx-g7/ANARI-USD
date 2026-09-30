// Copyright 2020 The Khronos Group
// SPDX-License-Identifier: Apache-2.0

#ifndef UsdBridgeRenderSettingsState_h
#define UsdBridgeRenderSettingsState_h

#include "usd.h"

#include <pxr/usd/usdRender/product.h>
#include <pxr/usd/usdRender/settings.h>
#include <pxr/usd/usdRender/var.h>

//
// Owns UsdRenderSettings / UsdRenderProduct / RenderVar prims for one frame.
// Managed by UsdRenderManager; render contexts do not interact with this class.
//
class UsdBridgeRenderSettingsState
{
public:
    void InitializePaths(const pxr::SdfPath& contextId);
    void CreatePrims(pxr::UsdStageRefPtr stage);
    void RemovePrims(pxr::UsdStageRefPtr stage);
    bool SetResolution(uint32_t width, uint32_t height);
    bool SetCameraPath(const pxr::SdfPath& cameraPath);
    const pxr::SdfPath& GetContextPath() const;

private:
    pxr::SdfPath ContextPath;
    pxr::SdfPath SettingsPath;
    pxr::SdfPath ProductPath;
    pxr::SdfPath VarPath;

    pxr::UsdRenderSettings Settings;
    pxr::UsdRenderProduct Product;

    uint32_t CachedWidth = 0;
    uint32_t CachedHeight = 0;
    pxr::SdfPath CachedCameraPath;
};

#endif
