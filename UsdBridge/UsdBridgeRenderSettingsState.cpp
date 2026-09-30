// Copyright 2020 The Khronos Group
// SPDX-License-Identifier: Apache-2.0

#include "UsdBridgeRenderSettingsState.h"
#include "UsdBridgeTimeEvaluator.h"
#include "UsdBridgeUsdWriter_Common.h"

#include <pxr/base/vt/value.h>

#include <cassert>

PXR_NAMESPACE_USING_DIRECTIVE

void UsdBridgeRenderSettingsState::InitializePaths(const SdfPath& contextId)
{
    ContextPath = SdfPath::AbsoluteRootPath().AppendChild(UsdBridgeRenderTokens->Render)
      .AppendChild(contextId.GetNameToken());
    SettingsPath = ContextPath.AppendChild(UsdBridgeRenderTokens->Settings);
    ProductPath = ContextPath.AppendChild(UsdBridgeRenderTokens->Product);
    VarPath = ContextPath.AppendChild(UsdBridgeRenderTokens->Vars)
      .AppendChild(UsdBridgeRenderTokens->LdrColor);
}

const SdfPath& UsdBridgeRenderSettingsState::GetContextPath() const
{
    return ContextPath;
}

void UsdBridgeRenderSettingsState::CreatePrims(UsdStageRefPtr stage)
{
    if (!stage)
    {
        return;
    }

    Settings = GetOrDefinePrim<UsdRenderSettings>(stage, SettingsPath);
    Settings.CreateResolutionAttr();
    Settings.CreateCameraRel();

    Product = GetOrDefinePrim<UsdRenderProduct>(stage, ProductPath);
    Product.CreateResolutionAttr();
    Product.CreateCameraRel();
    Product.CreateOrderedVarsRel();

    UsdRenderVar renderVarPrim = GetOrDefinePrim<UsdRenderVar>(stage, VarPath);
    renderVarPrim.CreateSourceNameAttr(
      VtValue(UsdBridgeRenderTokens->LdrColor.GetString()));
    Product.GetOrderedVarsRel().AddTarget(VarPath);
}

void UsdBridgeRenderSettingsState::RemovePrims(UsdStageRefPtr stage)
{
    if (!stage || ContextPath.IsEmpty())
    {
        return;
    }

    Settings = UsdRenderSettings();
    Product = UsdRenderProduct();
    stage->RemovePrim(ContextPath);
}

bool UsdBridgeRenderSettingsState::SetResolution(uint32_t width, uint32_t height)
{
    if (width == CachedWidth && height == CachedHeight)
        return false;

    CachedWidth = width;
    CachedHeight = height;

    if (Settings)
        Settings.GetResolutionAttr().Set(GfVec2i((int)width, (int)height));
    if (Product)
        Product.GetResolutionAttr().Set(GfVec2i((int)width, (int)height));

    return true;
}

bool UsdBridgeRenderSettingsState::SetCameraPath(const SdfPath& cameraPath)
{
    if (cameraPath == CachedCameraPath)
        return false;

    CachedCameraPath = cameraPath;

    if (Settings)
    {
        Settings.GetCameraRel().ClearTargets(false);
        Settings.GetCameraRel().AddTarget(cameraPath);
    }
    if (Product)
    {
        Product.GetCameraRel().ClearTargets(false);
        Product.GetCameraRel().AddTarget(cameraPath);
    }

    return true;
}
