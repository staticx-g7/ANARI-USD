#pragma once

#include "Common/UsdBridgeData.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace anari_usd_rawgeo
{
// RawGeo point attributes are float32. A particle ID can exceed the exact
// integer range of one float, so it is stored as one or more exact 16-bit
// chunks. The chunk count is a property of the source integer type; keeping
// it type-derived makes decoding independent of the values in a particular
// sample.
struct ParticleIdEncoding
{
  uint32_t Components = 0;
  uint32_t Bytes = 0;
};

inline ParticleIdEncoding GetParticleIdEncoding(UsdBridgeType Type)
{
  switch(Type)
  {
  case UsdBridgeType::UCHAR:
  case UsdBridgeType::CHAR:
    return {1, 1};
  case UsdBridgeType::USHORT:
  case UsdBridgeType::SHORT:
    return {1, 2};
  case UsdBridgeType::UINT:
  case UsdBridgeType::INT:
    return {2, 4};
  case UsdBridgeType::ULONG:
  case UsdBridgeType::LONG:
    return {4, 8};
  case UsdBridgeType::FLOAT:
    return {2, 4};
  case UsdBridgeType::DOUBLE:
    return {4, 8};
  default:
    return {};
  }
}

namespace detail
{
inline uint64_t ReadParticleId(const void* Ids, uint64_t Index, UsdBridgeType Type)
{
  const auto* Base = static_cast<const uint8_t*>(Ids);
  const uint8_t* P = Base + Index * GetParticleIdEncoding(Type).Bytes;

  switch(Type)
  {
  case UsdBridgeType::UCHAR: {
    uint8_t V = 0;
    std::memcpy(&V, P, sizeof(V));
    return V;
  }
  case UsdBridgeType::CHAR: {
    int8_t V = 0;
    std::memcpy(&V, P, sizeof(V));
    return static_cast<uint64_t>(static_cast<int64_t>(V));
  }
  case UsdBridgeType::USHORT: {
    uint16_t V = 0;
    std::memcpy(&V, P, sizeof(V));
    return V;
  }
  case UsdBridgeType::SHORT: {
    int16_t V = 0;
    std::memcpy(&V, P, sizeof(V));
    return static_cast<uint64_t>(static_cast<int64_t>(V));
  }
  case UsdBridgeType::UINT: {
    uint32_t V = 0;
    std::memcpy(&V, P, sizeof(V));
    return V;
  }
  case UsdBridgeType::INT: {
    int32_t V = 0;
    std::memcpy(&V, P, sizeof(V));
    return static_cast<uint64_t>(static_cast<int64_t>(V));
  }
  case UsdBridgeType::ULONG: {
    uint64_t V = 0;
    std::memcpy(&V, P, sizeof(V));
    return V;
  }
  case UsdBridgeType::LONG: {
    int64_t V = 0;
    std::memcpy(&V, P, sizeof(V));
    return static_cast<uint64_t>(V);
  }
  case UsdBridgeType::FLOAT: {
    float V = 0.0f;
    std::memcpy(&V, P, sizeof(V));
    return static_cast<uint64_t>(V);
  }
  case UsdBridgeType::DOUBLE: {
    double V = 0.0;
    std::memcpy(&V, P, sizeof(V));
    return static_cast<uint64_t>(V);
  }
  default:
    return 0;
  }
}
} // namespace detail

// Appends packed IDs to Out. SourceStride selects every Nth source ID, which
// matches the RawGeo position/attribute stride. Returns false and leaves Out
// unchanged for unsupported types.
inline bool PackParticleIds(const void* Ids, UsdBridgeType Type,
                            uint64_t SourceCount, uint64_t SourceStride,
                            std::vector<float>& Out)
{
  const ParticleIdEncoding Encoding = GetParticleIdEncoding(Type);
  if(!Ids || Encoding.Components == 0 || SourceStride == 0)
    return false;

  const size_t Start = Out.size();
  const uint64_t Count = (SourceCount + SourceStride - 1) / SourceStride;
  Out.resize(Start + static_cast<size_t>(Count) * Encoding.Components);

  size_t Cursor = Start;
  for(uint64_t i = 0; i < SourceCount; i += SourceStride)
  {
    const uint64_t Id = detail::ReadParticleId(Ids, i, Type);
    for(uint32_t Component = 0; Component < Encoding.Components; ++Component)
    {
      Out[Cursor++] = static_cast<float>((Id >> (16u * Component)) & 0xFFFFu);
    }
  }
  return true;
}
} // namespace anari_usd_rawgeo
