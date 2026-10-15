// Copyright 2020 The Khronos Group

#ifndef UsdBridgeAnimPlayer_h
#define UsdBridgeAnimPlayer_h

#include "UsdBridgeData.h"

#include <cstdio>
#include <typeinfo>

class UsdBridge;
struct UsdBridgePrimCache;

namespace jusync_anim
{
  bool MaybeRegister(UsdBridge* bridge, UsdGeometryHandle geometry,
      const char* primName, const UsdBridgeInstancerData& data, double timeStep);

  template<typename GeomDataType>
  inline bool MaybeRegister(UsdBridge*, UsdGeometryHandle, const char* n, const GeomDataType&, double)
  {
    fprintf(stderr, "[JusyncAnim] commit '%s' non-instancer type=%s (pass-through)\n",
        n ? n : "?", typeid(GeomDataType).name());
    return false;
  }

  void OnPrimDeleted(const UsdBridgePrimCache* primCache);

  bool IsStaleVariantMember(UsdGeometryHandle geometry);

  void Tick();
}

#endif
