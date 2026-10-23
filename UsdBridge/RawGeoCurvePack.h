#pragma once

// RawGeo v3 curve packing for the sender side.
//
// This is deliberately header-only and free of ANARI/USD includes: the broker
// writer and a plain host test both build the exact bytes the UE client reads.
// The input points/colors/widths are already the *selected* subset's curves, in
// ordinal order; `CurveLengths[i]` partitions them for the i-th selected curve.

#include "../AnariUsdProtocol.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace jusync_rawgeo
{
struct CurveAttribute
{
    const char* Name = nullptr;
    uint32_t Components = 0;
    const float* Data = nullptr;
};

struct CurvePackInput
{
    const char* PrimName = nullptr;
    double Time = 0.0;
    const float* Points = nullptr;          // selected curves, xyz per node
    uint64_t NumPoints = 0;
    const float* Colors = nullptr;          // ColorComponents per node, or null
    uint32_t ColorComponents = 0;           // 0, 3 or 4
    const float* Widths = nullptr;          // one float per node, or null
    const int32_t* CurveLengths = nullptr;  // NumCurves entries, each >= 2
    uint64_t NumCurves = 0;                 // curves in THIS payload
    const CurveAttribute* Attributes = nullptr;
    uint32_t NumAttributes = 0;             // <= 16
    uint64_t SubsetIndex = 0;
    uint64_t SubsetCount = 1;
    uint64_t FullCurveCount = 0;            // 0 => NumCurves
    uint64_t FullNumPoints = 0;             // 0 => NumPoints only when SubsetCount == 1
};

inline bool PackCurvePayload(const CurvePackInput& In, std::vector<uint8_t>& Out)
{
    using namespace anari_usd_protocol;

    Out.clear();

    if (In.PrimName == nullptr || In.SubsetCount == 0 || In.SubsetIndex >= In.SubsetCount ||
        In.SubsetCount > 8)
    {
        return false;
    }
    if (In.ColorComponents != 0 && In.ColorComponents != 3 && In.ColorComponents != 4)
    {
        return false;
    }
    if (In.NumAttributes > 16 || (In.NumAttributes > 0 && In.Attributes == nullptr))
    {
        return false;
    }

    const uint64_t FullCurves = In.FullCurveCount != 0 ? In.FullCurveCount : In.NumCurves;
    // The writer always supplies the full count for a subset. Defaulting it to
    // one subset's count would stamp a different C on each file.
    if (In.SubsetCount > 1 && In.FullCurveCount == 0)
    {
        return false;
    }
    // Match the client's allocation ceiling so an oversized frame falls back to
    // the USD path instead of being stored and rejected after download.
    if (FullCurves > 4194304ull)
    {
        return false;
    }
    if (In.NumCurves > FullCurves)
    {
        return false;
    }
    if (In.SubsetCount == 1)
    {
        if (In.SubsetIndex != 0 || In.NumCurves != FullCurves)
        {
            return false;
        }
    }
    else
    {
        const uint64_t Expected = FullCurves > In.SubsetIndex
            ? (FullCurves - In.SubsetIndex + In.SubsetCount - 1) / In.SubsetCount
            : 0;
        if (In.NumCurves != Expected)
        {
            return false;
        }
    }

    if (In.NumCurves > 0 && In.CurveLengths == nullptr)
    {
        return false;
    }

    uint64_t SumPoints = 0;
    for (uint64_t i = 0; i < In.NumCurves; ++i)
    {
        if (In.CurveLengths[i] < 2)
        {
            return false;
        }
        SumPoints += static_cast<uint64_t>(In.CurveLengths[i]);
    }
    if (SumPoints != In.NumPoints)
    {
        return false;
    }

    // For a subset, the full-frame point total is the client's merge/budget
    // hint. It cannot be derived from one subset, and a subset total would make
    // the client's `full_curve_count <= full_num_points / 2` guard reject a
    // legitimate padded frame.
    const uint64_t FullPoints = In.FullNumPoints != 0 ? In.FullNumPoints : SumPoints;
    if (In.SubsetCount > 1 && In.FullNumPoints == 0)
    {
        return false;
    }
    if (FullPoints < SumPoints || FullCurves > FullPoints / 2)
    {
        return false;
    }
    if (SumPoints > 0 && In.Points == nullptr)
    {
        return false;
    }
    if (SumPoints > 0 && In.ColorComponents != 0 && In.Colors == nullptr)
    {
        return false;
    }
    if (SumPoints > 0xFFFFFFFFull || In.NumCurves > 0xFFFFFFFFull || FullCurves > 0xFFFFFFFFull)
    {
        return false;
    }

    uint64_t AttrComponents = 0;
    for (uint32_t a = 0; a < In.NumAttributes; ++a)
    {
        const CurveAttribute& Attr = In.Attributes[a];
        if (Attr.Name == nullptr || Attr.Components < 1 || Attr.Components > 4)
        {
            return false;
        }
        if (SumPoints > 0 && Attr.Data == nullptr)
        {
            return false;
        }
        AttrComponents += Attr.Components;
    }

    const uint64_t ColorBytes = SumPoints * In.ColorComponents * sizeof(float);
    const uint64_t AttrBytes = SumPoints * AttrComponents * sizeof(float);
    const uint64_t CurveBytes = In.NumCurves * sizeof(uint32_t);
    const uint64_t WidthBytes = In.Widths != nullptr ? SumPoints * sizeof(float) : 0;
    const uint64_t PayloadBytes =
        SumPoints * 3 * sizeof(float) + ColorBytes + AttrBytes + CurveBytes + WidthBytes;

    JusyncRawGeoHeader Header{};
    Header.magic = JUSYNC_RAWGEO_MAGIC;
    Header.version = JUSYNC_RAWGEO_VERSION;
    std::strncpy(Header.prim_name, In.PrimName, sizeof(Header.prim_name) - 1);
    Header.num_points = SumPoints;
    Header.time = In.Time;
    Header.position_components = 3;
    Header.color_components = In.ColorComponents;
    Header.num_attributes = In.NumAttributes;
    Header.attr_uniform_count = static_cast<uint32_t>(SumPoints);
    Header.payload_bytes = PayloadBytes;
    Header.subset_index = In.SubsetIndex;
    Header.subset_count = In.SubsetCount;
    Header.full_num_points = FullPoints;
    Header.topology = JUSYNC_RAWGEO_TOPOLOGY_LINEAR_CURVES;
    Header.curve_count = static_cast<uint32_t>(In.NumCurves);
    Header.full_curve_count = FullCurves;
    Header.width_components = In.Widths != nullptr ? 1 : 0;
    Header.flags = 0;

    Out.resize(sizeof(Header) + sizeof(JusyncRawGeoAttrDesc) * In.NumAttributes + PayloadBytes);
    uint8_t* Cursor = Out.data();
    std::memcpy(Cursor, &Header, sizeof(Header));
    Cursor += sizeof(Header);

    for (uint32_t a = 0; a < In.NumAttributes; ++a)
    {
        JusyncRawGeoAttrDesc Desc{};
        std::strncpy(Desc.name, In.Attributes[a].Name, sizeof(Desc.name) - 1);
        Desc.components = In.Attributes[a].Components;
        Desc.pad = 0;
        std::memcpy(Cursor, &Desc, sizeof(Desc));
        Cursor += sizeof(Desc);
    }

    if (SumPoints > 0)
    {
        std::memcpy(Cursor, In.Points, SumPoints * 3 * sizeof(float));
        Cursor += SumPoints * 3 * sizeof(float);

        if (ColorBytes > 0)
        {
            std::memcpy(Cursor, In.Colors, ColorBytes);
            Cursor += ColorBytes;
        }

        for (uint32_t a = 0; a < In.NumAttributes; ++a)
        {
            const uint64_t Bytes = SumPoints * In.Attributes[a].Components * sizeof(float);
            std::memcpy(Cursor, In.Attributes[a].Data, Bytes);
            Cursor += Bytes;
        }
    }

    for (uint64_t i = 0; i < In.NumCurves; ++i)
    {
        const uint32_t Length = static_cast<uint32_t>(In.CurveLengths[i]);
        std::memcpy(Cursor, &Length, sizeof(Length));
        Cursor += sizeof(Length);
    }

    if (WidthBytes > 0)
    {
        std::memcpy(Cursor, In.Widths, WidthBytes);
    }

    return Header.valid(Out.data(), Out.size());
}

// "a.geo" + (k=3, S=4) -> "a_s3.geo"; S=1 keeps the whole-frame name.
inline std::string AddSubsetSuffix(const std::string& GeoKey, uint64_t K, uint64_t S)
{
    if (S <= 1)
    {
        return GeoKey;
    }

    const std::string Suffix = "_s" + std::to_string(K);
    const size_t Dot = GeoKey.find_last_of('.');
    if (Dot == std::string::npos)
    {
        return GeoKey + Suffix;
    }
    return GeoKey.substr(0, Dot) + Suffix + GeoKey.substr(Dot);
}

// Every memory-store key that can belong to one clip file. The writer stores
// stage twins plus up to eight curve subsets (`_s0.geo` ... `_s7.geo`), and a
// prim removal must invalidate all of them or the broker keeps advertising a
// retired element. Inputs that already carry a subset suffix are normalized
// first, so removing any one piece purges the whole family. Only clip-like
// names with the "__<time>" marker expand to subsets.
inline std::vector<std::string> StoreKeysForFile(const std::string& RelativeName)
{
    std::vector<std::string> Keys;
    auto Add = [&Keys](const std::string& Key) {
        for (const std::string& Existing : Keys)
        {
            if (Existing == Key)
            {
                return;
            }
        }
        Keys.push_back(Key);
    };

    Add(RelativeName);

    auto EndsWith = [](const std::string& Value, const char* Suffix) {
        const size_t N = std::strlen(Suffix);
        return Value.size() >= N && Value.compare(Value.size() - N, N, Suffix) == 0;
    };

    std::string Base;
    bool bStageFile = false;
    if (EndsWith(RelativeName, ".usd"))
    {
        Base = RelativeName.substr(0, RelativeName.size() - 4);
        bStageFile = true;
    }
    else if (EndsWith(RelativeName, ".usda"))
    {
        Base = RelativeName.substr(0, RelativeName.size() - 5);
        bStageFile = true;
    }
    else if (EndsWith(RelativeName, ".geo"))
    {
        Base = RelativeName.substr(0, RelativeName.size() - 4);
        bStageFile = true;
    }

    if (!bStageFile)
    {
        return Keys;
    }

    auto StripSubset = [](const std::string& Value) {
        // Only strip a writer-generated clip subset: "__<time>_s<digits>".
        // A plain actuator name such as "foo_s2" must not be shortened.
        const size_t TimeSep = Value.rfind("__");
        if (TimeSep == std::string::npos)
        {
            return Value;
        }
        const size_t Pos = Value.find("_s", TimeSep + 2);
        if (Pos == std::string::npos || Pos + 2 >= Value.size())
        {
            return Value;
        }
        for (size_t i = Pos + 2; i < Value.size(); ++i)
        {
            if (Value[i] < '0' || Value[i] > '9')
            {
                return Value;
            }
        }
        return Value.substr(0, Pos);
    };

    const std::string SubsetBase = StripSubset(Base);

    // Always include the base stage twins. For an already-suffixed input this
    // adds the unsuffixed twins (not phantom "_s<k>.usd" files).
    Add(SubsetBase + ".usd");
    Add(SubsetBase + ".usda");
    Add(SubsetBase + ".geo");
    if (Base.find("__") != std::string::npos)
    {
        for (uint64_t K = 0; K < 8; ++K)
        {
            Add(SubsetBase + "_s" + std::to_string(K) + ".geo");
        }
    }
    return Keys;
}
} // namespace jusync_rawgeo
