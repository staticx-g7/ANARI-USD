// Copyright 2020 The Khronos Group
// SPDX-License-Identifier: Apache-2.0

#ifndef UsdBridgeAnimPlayer_h
#define UsdBridgeAnimPlayer_h

#include "UsdBridgeData.h"

#include <cstdio>
#include <typeinfo>

class UsdBridge;
struct UsdBridgePrimCache;

// JUSYNC time-sample accumulation + replay (all inside the usd device).
//
// ParaView/VTK's ANARI mapper never re-commits geometry on a time-step change
// (the render gate is actor-MTime based) and stamps every commit it does make
// with the pipeline's requested time (0). A time-varying display therefore
// reaches the device as a sequence of commits under ONE prim name, each
// carrying the frame data visible at that moment but all labeled t=0.
//
// This module turns that sequence into a real timeline: every commit whose
// payload differs (fnv1a checksum over points/colors/attributes) is deep-
// copied into the group's sample ring with a synthetic time
// JUSYNC_ANIM_T0 + seq * JUSYNC_ANIM_DT, and immediately re-published through
// the normal per-timestep clip / RawGeo store / ZMQ machinery under the first
// live member's prim key. Clients thus see one genuinely time-sampled prim
// fed live, sample by sample, as ParaView commits them.
//
// Once commits stop arriving, a wall-clock paced pump replays the ring at
// JUSYNC_ANIM_FPS so downstream clients keep cycling the animation without
// any further work in ParaView. Re-commits of identical data (eye toggles at
// the same clock value) are deduplicated; the first commit that arrives after
// the scene prims were destroyed re-seeds the recreated prim from the stored
// ring.
//
// A single unique commit (static cloud) behaves exactly as the plain store
// path did before, just published synchronously on the commit itself.
//
// Env:
//   JUSYNC_ANIM_DETECT=0     disable (commits flow to the normal store)
//   JUSYNC_ANIM_FPS          replay publish rate (default 4)
//   JUSYNC_ANIM_T0           time of first sample (default 0.0)
//   JUSYNC_ANIM_DT           time delta between samples (default 1.0)
//   JUSYNC_ANIM_MAXSAMPLES   sample ring capacity (default 32)
namespace jusync_anim
{
  // Called on every geometry commit BEFORE the RawGeo store. Returns true if
  // this commit was absorbed into the timeline accumulator and must not be
  // stored as-is by the caller. Only float3-point instancer commits that
  // carry no exotic member data qualify; everything else passes through.
  bool MaybeRegister(UsdBridge* bridge, UsdGeometryHandle geometry,
      const char* primName, const UsdBridgeInstancerData& data, double timeStep);

  template<typename GeomDataType>
  inline bool MaybeRegister(UsdBridge*, UsdGeometryHandle, const char* n, const GeomDataType&, double)
  {
    fprintf(stderr, "[JusyncAnim] commit '%s' non-instancer type=%s (pass-through)\n",
        n ? n : "?", typeid(GeomDataType).name());
    return false;
  }

  // Stops watching a prim that is about to be deleted (its VTK-owned arrays
  // die with it). Accumulated samples are device-owned and survive.
  void OnPrimDeleted(const UsdBridgePrimCache* primCache);

  // Wall-clock paced: classifies settled groups and replays their sample
  // rings. Call from the device renderFrame after the scene save and before
  // the ZMQ notification dispatch.
  void Tick();
}

#endif
