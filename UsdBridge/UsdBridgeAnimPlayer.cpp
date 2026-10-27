// Copyright 2020 The Khronos Group

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
#include <utility>
#include <vector>

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
    int Uniq = -1;
  };

  // One timeline frame. Owns every byte it publishes.

  struct Sample
  {
    UsdBridgeInstancerData Data{}; // repointed into Buffers/AttrNames
    std::vector<uint8_t> Buffers;  // points | colors | float attributes
    std::vector<UsdBridgeAttribute> Attrs;
    std::vector<std::string> Names;
    uint64_t Checksum = 0;
    double Time = -1.0; // <0: not known yet, waiting for the renderer's usd::time
    int WaitTicks = 0;  // Tick() calls spent waiting for that time
  };

  struct Group
  {
    std::string Prefix;
    std::vector<Part*> Members; // live VTK geometries we own samples for, registration order
    std::vector<Part*> AllMembers;

    std::deque<Sample> Samples; // accumulated timeline (device-owned)
    size_t Seq = 0;             // next time slot: unseen payloads take t0 + Seq*dt
    std::vector<std::pair<uint64_t, double>> TimeHistory; // checksum -> first time seen
    std::chrono::steady_clock::time_point LastPayloadAt{}; // last commit seen (any path)
  std::chrono::steady_clock::time_point MembersEmptySince{}; // when the last member died ({} = live)
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

  std::vector<Group*> g_pending;

  std::chrono::steady_clock::time_point g_clock{};
  double g_frameDt = 0.25;
  double g_time0 = 0.0;
  double g_timeDt = 1.0;
  size_t g_maxSamples = 32;
  bool g_envRead = false;

  double g_publishDt = 0.0;
  std::chrono::steady_clock::time_point g_lastAccepted{};
  size_t g_timeCap = 512; // checksum->time history entries per group

  // Dataset time reported by the renderer as usd::time on the frame (or on the
  // device). When present it is the authoritative time: sample times no longer
  // come from a frame counter, so JUSYNC_ANIM_DT, skipped steps and scrubbing
  // cannot desynchronise Unreal from ParaView.
  double g_frameTime = -1.0;
  bool g_frameTimeSeen = false;
  double g_lastStampedTime = -1.0;

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
    if(const char* v = getenv("JUSYNC_ANIM_TIMECAP"))
      if(v[0]) g_timeCap = (size_t)atol(v);
    g_clock = std::chrono::steady_clock::now();
    fprintf(stderr, "[JusyncAnim] module v13 (replace-in-place, timed key) fps=%.3g t0=%.3g dt=%.3g cap=%zu timecap=%zu\n",
        1.0 / g_frameDt, g_time0, g_timeDt, g_maxSamples, g_timeCap);
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

  bool ParseSpheresName(const char* name, std::string& prefix, int64_t& idx, int& OutUniq)
  {
    OutUniq = -1;
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
    if(end < s.size() && s.compare(end, 2, "_u") == 0)
    {
      size_t u = end + 2;
      if(u >= s.size())
        return false;
      const size_t uniqStart = u;
      while(u < s.size() && s[u] >= '0' && s[u] <= '9')
        ++u;
      if(u == uniqStart)
        return false;
      OutUniq = static_cast<int>(strtol(s.c_str() + uniqStart, nullptr, 10));
      end = u;
    }
    if(end != s.size())
      return false;
    prefix = s.substr(0, pos);
    idx = strtoll(s.c_str() + digits, nullptr, 10);
    return true;
  }

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

  void Publish(Group& g, Sample& s, const char* tag)
  {
    if(s.Time < 0.0)
      return; // still waiting for the renderer's usd::time
    Part* key = nullptr;
    for(Part* m : g.Members)
      if(m && (!key || m->Uniq > key->Uniq))
        key = m;
    if(!key)
      return;
    if(key != g.Members.front())
      fprintf(stderr, "[JusyncAnim] publish key '%s' (uniq %d) over first member '%s'\n",
          key->Name.c_str(), key->Uniq, g.Members.front()->Name.c_str());
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

void SetFrameTime(double t)
{
  if(!DetectEnabled() || !(t > 0.0))
    return;

  std::lock_guard<std::recursive_mutex> lk(g_mutex);
  if(!g_frameTimeSeen)
  {
    g_frameTimeSeen = true;
    fprintf(stderr, "[JusyncAnim] dataset time source: usd::time=%.6g (exact pipeline time; "
        "JUSYNC_ANIM_DT is ignored)\n", t);
  }
  g_frameTime = t;
}

bool MaybeRegister(UsdBridge* bridge, UsdGeometryHandle geometry,
    const char* primName, const UsdBridgeInstancerData& data, double timeStep)
{
  if(g_inPublish)
    return false; // our own re-published sample - let the store run

  if(!DetectEnabled() || data.NumPoints == 0)
    return false;

  std::string prefix;
  int64_t idx = -1;
  int uniq = -1;
  if(!ParseSpheresName(primName, prefix, idx, uniq))
    return false; // not an instancer part - normal path

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
    p->Uniq = uniq;
    g_parts.push_back(p);
  }

  Group& g = FindGroup(prefix);
  if(std::find(g.AllMembers.begin(), g.AllMembers.end(), p) == g.AllMembers.end())
    g.AllMembers.push_back(p);

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

  if(std::find(g.Members.begin(), g.Members.end(), p) == g.Members.end())
    g.Members.push_back(p);

  // Clock reset decided HERE (not in OnPrimDeleted): a per-step _u swap
  // leaves the group empty only for the ms between the old prim's release
  // and this registration, even under multi-second slow-render latency;
  // a dataset swap leaves it empty through the user's reload (tens of s).
  // The empty window's duration is the structural discriminator.
  if(g.MembersEmptySince.time_since_epoch().count() != 0)
  {
    const auto nowReg = std::chrono::steady_clock::now();
    const bool idleSwap =
        std::chrono::duration<double>(nowReg - g.MembersEmptySince).count() > 2.0;
    g.MembersEmptySince = {};
    if(idleSwap)
    {
      g.TimeHistory.clear();
      g.Seq = 0; // new dataset: the timeline restarts at t0
    }
  }

  const uint64_t cksum = ChecksumOf(data);

  bool seen = false;
  for(const Sample& s : g.Samples)
    if(s.Checksum == cksum) { seen = true; break; }
  const bool newPayload = !seen;

  fprintf(stderr, "[JusyncAnim] part '%s' pts=%llu t=%.6g%s\n", p->Name.c_str(),
      (unsigned long long)data.NumPoints, timeStep,
      newPayload ? " <- NEW" : " <- dup");

  if(newPayload && g_publishDt > 0.0)
  {
    const auto nowAccepted = std::chrono::steady_clock::now();
    g.LastPayloadAt = nowAccepted; // stream activity, even if we drop this one
    if(g_lastAccepted.time_since_epoch().count() != 0 &&
       std::chrono::duration<double>(nowAccepted - g_lastAccepted).count() < g_publishDt)
    {
      // Rate limited: this payload is not published, but the dataset time slot
      // it belongs to is real. Advance the clock so the samples that do get
      // published keep the dataset's own time instead of drifting by the drop
      // ratio (PUBLISH_HZ < ANIM_FPS would otherwise compress the timeline).
      ++g.Seq;
      g.Touched = true;
      return true;
    }
    g_lastAccepted = nowAccepted;
  }

  if(newPayload)
  {
    g.LastPayloadAt = std::chrono::steady_clock::now();

    // Exact dataset time for this payload, in order of preference:
    //   1. a caller that already knows the time (usd::time > 0 on the geometry
    //      or on the device, e.g. the C demo) wins;
    //   2. the frame's usd::time, which the renderer reports per render - the
    //      sample is left unstamped here (t < 0) and Tick() stamps it with the
    //      exact pipeline time. This is what makes Unreal match ParaView for
    //      any time step, including irregular ones and scrubbing;
    //   3. a checksum seen before (loop or scrub-back revisit) reuses its FIRST
    //      time, so clip names and UE sample times stay stable across revisits;
    //   4. otherwise the next slot t0 + n*dt. Only used when no renderer time
    //      is available at all, which is why JUSYNC_ANIM_DT has to match the
    //      dataset step.
    double t = timeStep > 0.0 ? timeStep : -1.0;
    if(t < 0.0 && !g_frameTimeSeen)
    {
      for(const auto& hc : g.TimeHistory)
        if(hc.first == cksum) { t = hc.second; break; }
    }
    if(t < 0.0 && !g_frameTimeSeen)
    {
      t = g_time0 + double(g.Seq) * g_timeDt;
      ++g.Seq;
      if(g.TimeHistory.size() < g_timeCap)
        g.TimeHistory.emplace_back(cksum, t);
    }

    g.Samples.clear();
    g.Pending.clear();
    g.Mode = 0;
    g.Samples.emplace_back();
    Sample& s = g.Samples.back();
    Capture(s, data, cksum, t);
    if(data.NumPoints > g.MaxPoints)
      g.MaxPoints = data.NumPoints;
    g.LastChecksum = cksum;
    g.Touched = true;
    g.NeedReseed = false;
    g.Pending.push_back(0); // published from Tick(), not here
    return true;
  }

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
        auto itAll = std::find(g->AllMembers.begin(), g->AllMembers.end(), p);
        if(itAll != g->AllMembers.end())
          g->AllMembers.erase(itAll);
        if(g->Members.empty() && !g->Samples.empty())
        {
          g->NeedReseed = true; // prims are gone - next commit re-seeds
          if(g->Mode == 1)
            g->Mode = 0;        // static prim rebuilt: re-evaluate samples
          // VTK swaps the spheres prim on every timestep, so an empty member
          // list alone is NOT a new-dataset signal. Do NOT reset the clock
          // here: under slow (software) rendering the old prim is released
          // >2 s after the last payload and before the next registration,
          // so deletion-time quiet checks reset the timeline EVERY step
          // (all frames collapse to t=0). Just stamp when we went empty;
          // MaybeRegister decides from the empty window's DURATION, which
          // is ~ms for step swaps and tens of seconds for dataset swaps.
          if(g->MembersEmptySince.time_since_epoch().count() == 0)
            g->MembersEmptySince = std::chrono::steady_clock::now();
        }
        g->Touched = true;
      }
      g_parts.erase(g_parts.begin() + i);
      delete p;
      return;
    }
  }
}

bool IsStaleVariantMember(UsdGeometryHandle geometry)
{
  if(g_inPublish || !DetectEnabled() || geometry.value == nullptr)
    return false;

  std::lock_guard<std::recursive_mutex> lk(g_mutex);

  const Part* p = FindPart(geometry.value);
  if(!p || p->Uniq < 0)
    return false;

  for(Group* g : g_groups)
    for(const Part* q : g->AllMembers)
      if(q && q != p && q->SpheresIdx == p->SpheresIdx && q->Uniq > p->Uniq)
        return true;
  return false;
}

void Tick()
{
  if(!DetectEnabled())
    return;

  std::lock_guard<std::recursive_mutex> lk(g_mutex);
  ReadEnv();

  // Stamp queued samples with the exact dataset time the renderer reported as
  // usd::time. The new payload was committed earlier in this render, before the
  // frame's time reached us, so the time is applied here, right before publish.
  for(Group* g : g_groups)
  {
    if(g->Pending.empty())
      continue;
    for(size_t si : g->Pending)
    {
      if(si >= g->Samples.size())
        continue;
      Sample& s = g->Samples[si];
      if(s.Time >= 0.0)
        continue;
      if(g_frameTimeSeen && g_frameTime > 0.0 && g_frameTime != g_lastStampedTime)
      {
        s.Time = g_frameTime;
        g_lastStampedTime = g_frameTime;
      }
      else if(++s.WaitTicks >= 3)
      {
        // The renderer never supplied a time: fall back to the slot counter so
        // the stream keeps moving (this is the old behaviour).
        s.Time = g_time0 + double(g->Seq) * g_timeDt;
        ++g->Seq;
      }
    }
  }

  for(Group* g : g_groups)
  {
    if(g->Pending.empty())
      continue;
    if(g->Members.empty())
      continue; // no live key prim yet - keep this group queued, do not
    std::vector<size_t> keep;
    for(size_t si : g->Pending)
    {
      if(si >= g->Samples.size())
        continue;
      if(g->Samples[si].Time >= 0.0)
        Publish(*g, g->Samples[si], "sample+");
      else
        keep.push_back(si);
    }
    g->Pending.swap(keep);
  }

  const auto now = std::chrono::steady_clock::now();

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
