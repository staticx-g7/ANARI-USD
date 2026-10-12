// Copyright 2020 The Khronos Group
// SPDX-License-Identifier: Apache-2.0

#include "UsdBridgeAnimPlayer.h"

#include "UsdBridge.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

// JUSYNC time-sample accumulation + replay, see UsdBridgeAnimPlayer.h.
//
// Commits that VTK re-issues for a time-varying single prim (all stamped
// t=0) are accumulated here as deep-copied timeline samples keyed by payload
// checksum, published live through the per-timestep store machinery, and
// replayed on a wall-clock pump once ParaView stops committing.
namespace jusync_anim
{

namespace
{
  struct Part
  {
    UsdBridge* Bridge = nullptr;
    UsdGeometryHandle Geometry{};
    std::string Name;
    int64_t SpheresIdx = -1;
  };

  // One timeline frame. Owns every byte it publishes.
  struct Sample
  {
    UsdBridgeInstancerData Data{}; // repointed into Buffers/AttrNames
    std::vector<uint8_t> Buffers;  // points | colors | float attributes
    std::vector<UsdBridgeAttribute> Attrs;
    std::vector<std::string> Names;
    uint64_t Checksum = 0;
    double Time = 0.0;
  };

  struct Group
  {
    std::string Prefix;
    std::vector<Part*> Members; // live VTK geometries, registration order

    std::deque<Sample> Samples; // accumulated timeline (device-owned)
    size_t Seq = 0;             // monotonic sample counter -> synthetic time
    uint64_t LastChecksum = 0;
    uint64_t MaxPoints = 0;     // largest commit seen (streaming chunks are smaller)
    bool NeedReseed = false;    // all prims died; first commit republishes
    std::vector<size_t> Pending; // sample indices awaiting Tick() publish

    bool Touched = false;
    int StableTicks = 0;
    int Mode = 0; // 0 recording/pending, 1 static done, 2 replay timeline
    size_t Next = 0;
    std::chrono::steady_clock::time_point LastPublish{};
  };

  std::recursive_mutex g_mutex;
  std::vector<Part*> g_parts;
  std::vector<Group*> g_groups;

  thread_local bool g_inPublish = false; // sample -> normal store path

  // Samples captured but not yet stored: published from Tick() so the store
  // never runs re-entrantly inside VTK's own SetGeometryData call.
  std::vector<Group*> g_pending;

  std::chrono::steady_clock::time_point g_clock{};
  double g_frameDt = 0.25;
  double g_time0 = 0.0;
  double g_timeDt = 1.0;
  size_t g_maxSamples = 32;
  bool g_envRead = false;

  // Minimum wall-clock time between ACCEPTED frames (0 = unlimited). VTK can
  // commit far faster than the store/notify path can service while an animation
  // is played/looped; frames inside this window are dropped before Capture, so
  // a fast play loop costs no deep copy and no store/notify work.
  double g_publishDt = 0.0;
  std::chrono::steady_clock::time_point g_lastAccepted{};

  bool DetectEnabled()
  {
    static const bool enabled = [](){
      const char* v = getenv("JUSYNC_ANIM_DETECT");
      return !(v && v[0] == '0');
    }();
    return enabled;
  }

  void ReadEnv()
  {
    if(g_envRead)
      return;
    g_envRead = true;

    double fps = 4.0;
    if(const char* v = getenv("JUSYNC_ANIM_FPS"))
      if(v[0]) fps = atof(v);
    if(fps < 0.05) fps = 0.05;
    if(fps > 30.0) fps = 30.0;
    g_frameDt = 1.0 / fps;

    if(const char* v = getenv("JUSYNC_ANIM_T0"))
      if(v[0]) g_time0 = atof(v);
    if(const char* v = getenv("JUSYNC_ANIM_DT"))
      if(v[0]) g_timeDt = atof(v);
    if(const char* v = getenv("JUSYNC_ANIM_MAXSAMPLES"))
      if(v[0]) g_maxSamples = (size_t)atol(v);
    if(g_maxSamples < 2) g_maxSamples = 2;
    if(const char* v = getenv("JUSYNC_ANIM_PUBLISH_HZ"))
      if(v[0]) { const double hz = atof(v); if(hz > 0.0) g_publishDt = 1.0 / hz; }
    g_clock = std::chrono::steady_clock::now();
    fprintf(stderr, "[JusyncAnim] module v10 (replace-in-place, constant key) fps=%.3g t0=%.3g dt=%.3g cap=%zu\n",
        1.0 / g_frameDt, g_time0, g_timeDt, g_maxSamples);
  }

  uint32_t FloatComponents(UsdBridgeType type)
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

  size_t ElementBytes(UsdBridgeType t)
  {
    switch(t)
    {
      case UsdBridgeType::BOOL:
      case UsdBridgeType::UCHAR:
      case UsdBridgeType::CHAR:        return 1;
      case UsdBridgeType::USHORT:
      case UsdBridgeType::SHORT:
      case UsdBridgeType::HALF:        return 2;
      case UsdBridgeType::UINT:
      case UsdBridgeType::INT:
      case UsdBridgeType::FLOAT:       return 4;
      case UsdBridgeType::ULONG:
      case UsdBridgeType::LONG:
      case UsdBridgeType::DOUBLE:      return 8;
      case UsdBridgeType::FLOAT3:      return 12;
      case UsdBridgeType::FLOAT4:      return 16;
      case UsdBridgeType::DOUBLE3:     return 24;
      case UsdBridgeType::INT3:
      case UsdBridgeType::UINT3:       return 12;
      case UsdBridgeType::FLOAT2:      return 8;
      case UsdBridgeType::INT2:
      case UsdBridgeType::UINT2:       return 8;
      default:                         return 0;
    }
  }

  uint64_t Fnv(const void* p, size_t n, uint64_t h)
  {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for(size_t i = 0; i < n; ++i)
      h = (h ^ b[i]) * 0x100000001b3ULL;
    return h;
  }

  // "<prefix>_spheres_<n>" (optionally with a VTK uniquifier suffix "_u<k>")
  // -> split. Returns false for any other name.
  bool ParseSpheresName(const char* name, std::string& prefix, int64_t& idx)
  {
    if(!name)
      return false;
    const std::string s(name);
    size_t pos = s.rfind("_spheres_");
    if(pos == std::string::npos)
      return false;
    const size_t digits = pos + 9;
    if(digits >= s.size())
      return false;
    size_t end = digits;
    while(end < s.size() && s[end] >= '0' && s[end] <= '9')
      ++end;
    // tolerate trailing "_u1", "_u2", ... display-rebuild uniquifiers
    if(end < s.size() && s.compare(end, 2, "_u") == 0)
    {
      size_t u = end + 2;
      if(u >= s.size())
        return false;
      while(u < s.size() && s[u] >= '0' && s[u] <= '9')
        ++u;
      end = u;
    }
    if(end != s.size())
      return false;
    prefix = s.substr(0, pos);
    idx = strtoll(s.c_str() + digits, nullptr, 10);
    return true;
  }

  // Only point clouds qualify: float3 points, no per-instance transforms.
  // Per-instance Scales (sphere radius!) and InstanceIds (VTK's sphere mapper
  // always sends both) DO ride the copy.
  bool Streamable(const UsdBridgeInstancerData& d)
  {
    const bool scalesOk = !d.Scales || FloatComponents(d.ScalesType) > 0;
    const bool idsOk = !d.InstanceIds || ElementBytes(d.InstanceIdsType) > 0;
    return d.Points && d.NumPoints > 0 &&
        d.PointsType == UsdBridgeType::FLOAT3 && scalesOk && idsOk &&
        !d.ShapeIndices && !d.Orientations &&
        !d.InvisibleIds &&
        !d.LinearVelocities && !d.AngularVelocities &&
        (!d.Colors ||
            FloatComponents(d.ColorsType) == 3 ||
            FloatComponents(d.ColorsType) == 4);
  }

  uint64_t BytesOf(UsdBridgeType t, uint64_t n)
  {
    const uint32_t c = FloatComponents(t);
    return c ? n * c * sizeof(float) : 0;
  }

  // Checksum over everything the sample will carry.
  uint64_t ChecksumOf(const UsdBridgeInstancerData& d)
  {
    uint64_t h = 0xcbf29ce484222325ULL;
    h = Fnv(d.Points, d.NumPoints * 3 * sizeof(float), h);
    if(d.Scales)
      h = Fnv(d.Scales, BytesOf(d.ScalesType, d.NumPoints), h);
    if(d.InstanceIds)
      h = Fnv(d.InstanceIds, d.NumPoints * ElementBytes(d.InstanceIdsType), h);
    if(d.Colors)
      h = Fnv(d.Colors, d.NumPoints * FloatComponents(d.ColorsType) * sizeof(float), h);
    for(uint32_t a = 0; a < d.NumAttributes && d.Attributes; ++a)
    {
      const UsdBridgeAttribute& src = d.Attributes[a];
      const uint32_t comps = FloatComponents(src.DataType);
      if(!src.Data || src.PerPrimData || comps == 0)
        continue;
      h = Fnv(src.Data, d.NumPoints * comps * sizeof(float), h);
    }
    return h;
  }

  // Deep-copy the commit into a device-owned Sample (arrays die with the
  // VTK actor, the timeline must outlive it).
  void Capture(Sample& s, const UsdBridgeInstancerData& d, uint64_t cksum, double time)
  {
    const uint64_t n = d.NumPoints;
    const uint32_t posC = 3;
    const uint32_t colC = d.Colors ? FloatComponents(d.ColorsType) : 0;
    const uint32_t sclC = d.Scales ? FloatComponents(d.ScalesType) : 0;
    const size_t idsEB = d.InstanceIds ? ElementBytes(d.InstanceIdsType) : 0;

    std::vector<const UsdBridgeAttribute*> kept;
    uint64_t attrBytes = 0;
    for(uint32_t a = 0; a < d.NumAttributes && d.Attributes; ++a)
    {
      const UsdBridgeAttribute& src = d.Attributes[a];
      const uint32_t comps = FloatComponents(src.DataType);
      if(!src.Data || src.PerPrimData || comps == 0)
        continue;
      kept.push_back(&src);
      attrBytes += uint64_t(comps) * sizeof(float);
    }

    s.Buffers.clear();
    s.Buffers.resize(n * (posC + colC + sclC) * sizeof(float) + n * idsEB + n * attrBytes);
    uint8_t* w = s.Buffers.data();
    memcpy(w, d.Points, n * posC * sizeof(float));
    w += n * posC * sizeof(float);
    if(colC)
    {
      memcpy(w, d.Colors, n * colC * sizeof(float));
      w += n * colC * sizeof(float);
    }
    if(sclC)
    {
      memcpy(w, d.Scales, n * sclC * sizeof(float));
      w += n * sclC * sizeof(float);
    }
    if(idsEB)
    {
      memcpy(w, d.InstanceIds, n * idsEB);
      w += n * idsEB;
    }
    for(const UsdBridgeAttribute* src : kept)
    {
      const uint32_t comps = FloatComponents(src->DataType);
      memcpy(w, src->Data, n * comps * sizeof(float));
      w += n * comps * sizeof(float);
    }

    w = s.Buffers.data() + size_t(n * (posC + colC + sclC) * sizeof(float) + n * idsEB);
    s.Names.clear();
    s.Attrs.clear();
    for(uint32_t i = 0; i < kept.size(); ++i)
      s.Names.emplace_back(kept[i]->Name ? kept[i]->Name : "");
    for(const UsdBridgeAttribute* src : kept)
    {
      const uint32_t comps = FloatComponents(src->DataType);
      UsdBridgeAttribute a;
      a.Data = w;
      a.DataType = src->DataType;
      a.PerPrimData = false;
      a.EltSize = comps * sizeof(float);
      s.Attrs.push_back(a);
      w += n * comps * sizeof(float);
    }

    s.Data = d;
    s.Data.Points = s.Buffers.data();
    const uint8_t* afterPos = s.Buffers.data() + size_t(n * posC * sizeof(float));
    s.Data.Colors = colC ? static_cast<const void*>(afterPos) : nullptr;
    s.Data.Scales = sclC ? static_cast<const void*>(
        afterPos + size_t(n * colC * sizeof(float))) : nullptr;
    s.Data.InstanceIds = idsEB ? static_cast<const void*>(
        afterPos + size_t(n * (colC + sclC) * sizeof(float))) : nullptr;
    for(size_t i = 0; i < s.Attrs.size(); ++i)
      s.Attrs[i].Name = s.Names[i].c_str();
    s.Data.Attributes = s.Attrs.empty() ? nullptr : s.Attrs.data();
    s.Data.NumAttributes = uint32_t(s.Attrs.size());

    s.Checksum = cksum;
    s.Time = time;
  }

  Group& FindGroup(const std::string& prefix)
  {
    for(Group* g : g_groups)
      if(g->Prefix == prefix)
        return *g;
    Group* g = new Group();
    g->Prefix = prefix;
    g_groups.push_back(g);
    return *g;
  }

  // Re-publish one accumulated sample through the normal store path, under
  // the first live member's prim key (clients see a single time-varying prim).
  void Publish(Group& g, Sample& s, const char* tag)
  {
    if(g.Members.empty())
      return;
    Part* key = g.Members.front();
    fprintf(stderr, "[JusyncAnim] %s '%s' t=%.6g pts=%llu ck=%016llx\n", tag,
        g.Prefix.c_str(), s.Time, (unsigned long long)s.Data.NumPoints,
        (unsigned long long)s.Checksum);
    g_inPublish = true;
    key->Bridge->SetGeometryData(key->Geometry, s.Data, s.Time);
    g_inPublish = false;
  }

  Part* FindPart(const void* geomValue)
  {
    for(Part* p : g_parts)
      if(p->Geometry.value == geomValue)
        return p;
    return nullptr;
  }
} // anonymous namespace

bool MaybeRegister(UsdBridge* bridge, UsdGeometryHandle geometry,
    const char* primName, const UsdBridgeInstancerData& data, double timeStep)
{
  if(g_inPublish)
    return false; // our own re-published sample - let the store run

  if(!DetectEnabled() || data.NumPoints == 0)
    return false;

  std::string prefix;
  int64_t idx = -1;
  if(!ParseSpheresName(primName, prefix, idx))
    return false; // not an instancer part - normal path

  if(!Streamable(data))
  {
    fprintf(stderr, "[JusyncAnim] '%s' NOT streamable: pts=%p type=%d n=%llu shape=%d scales=%d "
        "orient=%d ids=%d invis=%d linv=%d angv=%d colors=%d\n", primName ? primName : "?",
        (const void*)data.Points, (int)data.PointsType, (unsigned long long)data.NumPoints,
        data.ShapeIndices ? 1 : 0, data.Scales ? 1 : 0, data.Orientations ? 1 : 0,
        data.InstanceIds ? 1 : 0, data.InvisibleIds ? 1 : 0,
        data.LinearVelocities ? 1 : 0, data.AngularVelocities ? 1 : 0,
        data.Colors ? 1 : 0);
    return false; // exotic instancer - normal path keeps old behavior
  }

  std::lock_guard<std::recursive_mutex> lk(g_mutex);
  ReadEnv();

  Part* p = FindPart(geometry.value);
  if(!p)
  {
    p = new Part();
    p->Bridge = bridge;
    p->Geometry = geometry;
    p->Name = primName ? primName : "";
    p->SpheresIdx = idx;
    g_parts.push_back(p);
  }

  Group& g = FindGroup(prefix);
  if(std::find(g.Members.begin(), g.Members.end(), p) == g.Members.end())
    g.Members.push_back(p);

  const uint64_t cksum = ChecksumOf(data);

  // Every distinct payload is the next time step. VTK/ParaView re-commits a
  // time-varying instancer with frames whose point count changes from step to
  // step (and alternates LOD / full resolution), so the timeline must accept
  // varying counts and must NEVER reset its synthetic clock: doing so stamped
  // every step t=0, collapsing the whole series onto one clip key and thus
  // suppressing every downstream time-step update.
  bool seen = false;
  for(const Sample& s : g.Samples)
    if(s.Checksum == cksum) { seen = true; break; }
  const bool newPayload = !seen;

  fprintf(stderr, "[JusyncAnim] part '%s' pts=%llu t=%.6g%s\n", p->Name.c_str(),
      (unsigned long long)data.NumPoints, timeStep,
      newPayload ? " <- NEW" : " <- dup");

  // Publish throttle: while an animation is playing/looped VTK commits as fast
  // as it can. Dropping frames inside the window here (before Capture) keeps the
  // render/GUI thread off the store+notify path entirely for the skipped frames.
  if(newPayload && g_publishDt > 0.0)
  {
    const auto nowAccepted = std::chrono::steady_clock::now();
    if(g_lastAccepted.time_since_epoch().count() != 0 &&
       std::chrono::duration<double>(nowAccepted - g_lastAccepted).count() < g_publishDt)
    {
      g.Touched = true;
      return true;
    }
    g_lastAccepted = nowAccepted;
  }

  if(newPayload)
  {
    // Replace-in-place: keep only the NEWEST sample and always publish it at
    // the same (constant) time so every commit OVERWRITES the single clip/
    // .geo key for this prim. Clients then update the existing cloud instead of
    // accumulating one actor/file per step, and the memory store stays bounded
    // (one key per prim). A distinct payload still changes the file hash, so a
    // V2 notification fires and the client refreshes in place.
    g.Samples.clear();
    g.Pending.clear();
    g.Seq = 0;
    g.Mode = 0;
    g.Samples.emplace_back();
    Sample& s = g.Samples.back();
    Capture(s, data, cksum, g_time0);
    if(data.NumPoints > g.MaxPoints)
      g.MaxPoints = data.NumPoints;
    g.LastChecksum = cksum;
    g.Touched = true;
    g.NeedReseed = false;
    g.Pending.push_back(0); // published from Tick(), not here
    return true;
  }

  // Duplicate payload: skip unless the scene prims were destroyed and the
  // recreated prim still needs its data (re-seed from the ring).
  if(g.NeedReseed && !g.Samples.empty())
  {
    g.NeedReseed = false;
    g.Touched = true;
    g.Pending.push_back(g.Samples.size() - 1);
  }
  return true;
}

void OnPrimDeleted(const UsdBridgePrimCache* primCache)
{
  std::lock_guard<std::recursive_mutex> lk(g_mutex);

  for(size_t i = 0; i < g_parts.size(); ++i)
  {
    if(g_parts[i]->Geometry.value == primCache)
    {
      Part* p = g_parts[i];
      fprintf(stderr, "[JusyncAnim] unwatch part '%s' (deleted)\n", p->Name.c_str());
      for(Group* g : g_groups)
      {
        auto it = std::find(g->Members.begin(), g->Members.end(), p);
        if(it != g->Members.end())
          g->Members.erase(it);
        if(g->Members.empty() && !g->Samples.empty())
        {
          g->NeedReseed = true; // prims are gone - next commit re-seeds
          if(g->Mode == 1)
            g->Mode = 0;        // static prim rebuilt: re-evaluate samples
        }
        g->Touched = true;
      }
      g_parts.erase(g_parts.begin() + i);
      delete p;
      return;
    }
  }
}

void Tick()
{
  if(!DetectEnabled())
    return;

  std::lock_guard<std::recursive_mutex> lk(g_mutex);
  ReadEnv();

  // Drain deferred stores first: samples captured during VTK commits are
  // published here, outside any in-flight SetGeometryData call.
  for(Group* g : g_groups)
  {
    if(g->Pending.empty())
      continue;
    if(g->Members.empty())
      continue; // no live key prim yet - keep this group queued, do not
                // stall the other groups' pending stores
    for(size_t si : g->Pending)
      if(si < g->Samples.size())
        Publish(*g, g->Samples[si], "sample+");
    g->Pending.clear();
  }

  const auto now = std::chrono::steady_clock::now();

  // Classify groups that stopped receiving commits.
  for(Group* g : g_groups)
  {
    if(g->Touched)
    {
      g->Touched = false;
      g->StableTicks = 0;
      continue;
    }
    g->StableTicks += 1;
    if(g->StableTicks >= 2 && g->Mode == 0 && !g->Samples.empty())
    {
      if(g->Samples.size() >= 2)
      {
        g->Mode = 2;
        g->Next = 0;
        g->LastPublish = std::chrono::steady_clock::time_point{};
        fprintf(stderr, "[JusyncAnim] TIMELINE '%s': %zu time samples, fps=%.3g\n",
            g->Prefix.c_str(), g->Samples.size(), 1.0 / g_frameDt);
      }
      else
      {
        g->Mode = 1; // single unique commit - static cloud, already stored
      }
    }
  }

  // Replay accumulated timelines, globally paced, while nothing is live.
  bool due = false;
  for(Group* g : g_groups)
    if(g->Mode == 2 && !g->Members.empty() && g->StableTicks > 0)
    {
      if(g->LastPublish.time_since_epoch().count() == 0 ||
         std::chrono::duration<double>(now - g->LastPublish).count() >= g_frameDt)
        due = true;
      break;
    }
  if(due)
  {
    for(Group* g : g_groups)
      if(g->Mode == 2 && !g->Members.empty() && g->StableTicks > 0)
      {
        g->LastPublish = now;
        Sample& s = g->Samples[g->Next % g->Samples.size()];
        ++g->Next;
        Publish(*g, s, "stream");
      }
  }

  // Forget groups whose prims are all gone and never came back.
  for(size_t i = 0; i < g_groups.size();)
  {
    if(g_groups[i]->Members.empty() && g_groups[i]->Samples.empty() &&
       g_groups[i]->StableTicks > 4)
    {
      delete g_groups[i];
      g_groups.erase(g_groups.begin() + i);
    }
    else
      ++i;
  }
}

} // namespace jusync_anim
