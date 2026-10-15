// Copyright 2020 The Khronos Group

#pragma once

#include "UsdBridgedBaseObject.h"

#include <limits>

class UsdGeometry;
class UsdMaterial;

struct UsdSurfaceData
{
  UsdSharedString* name = nullptr;
  UsdSharedString* usdName = nullptr;


  UsdGeometry* geometry = nullptr;
  UsdMaterial* material = nullptr;

  double geometryRefTimeStep = std::numeric_limits<float>::quiet_NaN();
  double materialRefTimeStep = std::numeric_limits<float>::quiet_NaN();

  bool isInstanceable = false;
};

class UsdSurface : public UsdBridgedBaseObject<UsdSurface, UsdSurfaceData, UsdSurfaceHandle>
{
  public:
    UsdSurface(const char* name, UsdDevice* device);
    ~UsdSurface();

    void remove(UsdDevice* device) override;

    bool isInstanceable() const;

  protected:
    bool deferCommit(UsdDevice* device) override;
    bool doCommitData(UsdDevice* device) override;
    void doCommitRefs(UsdDevice* device) override;

    void onParamRefChanged(UsdBaseObject* paramObject, bool incRef, bool onWriteParams) override;
    void observe(UsdBaseObject* caller, UsdDevice* device) override;

    bool updateBoundParameters = false;
};