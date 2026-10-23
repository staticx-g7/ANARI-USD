// Host test for the sender-side RawGeo particle ID packing. The ID is stored
// as exact 16-bit chunks in floats so the wire format stays v2-compatible.

#include "UsdBridge/RawGeoParticleId.h"
#include "UsdBridge/Common/UsdBridgeData.h"

#include <cstdint>
#include <cstdio>
#include <vector>

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); return 1; } } while (0)

namespace
{
uint64_t Decode(const std::vector<float>& Values, size_t Offset, uint32_t Components)
{
    uint64_t Id = 0;
    for (uint32_t Component = 0; Component < Components; ++Component)
    {
        const uint32_t Chunk = static_cast<uint32_t>(Values[Offset + Component]);
        Id |= static_cast<uint64_t>(Chunk & 0xFFFFu) << (16u * Component);
    }
    return Id;
}

bool ExpectEncoding(UsdBridgeType Type, uint32_t Components, uint32_t Bytes, const char* Label)
{
    const auto Encoding = anari_usd_rawgeo::GetParticleIdEncoding(Type);
    if (Encoding.Components != Components || Encoding.Bytes != Bytes)
    {
        std::fprintf(stderr, "FAIL: %s encoding got %u/%u expected %u/%u\n",
            Label, Encoding.Components, Encoding.Bytes, Components, Bytes);
        return false;
    }
    return true;
}
} // namespace

int main()
{
    CHECK(ExpectEncoding(UsdBridgeType::UCHAR, 1, 1, "UCHAR"), "UCHAR encoding");
    CHECK(ExpectEncoding(UsdBridgeType::USHORT, 1, 2, "USHORT"), "USHORT encoding");
    CHECK(ExpectEncoding(UsdBridgeType::UINT, 2, 4, "UINT"), "UINT encoding");
    CHECK(ExpectEncoding(UsdBridgeType::ULONG, 4, 8, "ULONG"), "ULONG encoding");
    CHECK(ExpectEncoding(UsdBridgeType::INT, 2, 4, "INT"), "INT encoding");
    CHECK(ExpectEncoding(UsdBridgeType::LONG, 4, 8, "LONG"), "LONG encoding");
    CHECK(!anari_usd_rawgeo::GetParticleIdEncoding(UsdBridgeType::FLOAT3).Components,
        "FLOAT3 is not an ID type");

    {
        const uint32_t Ids[] = { 0u, 65535u, 65536u, 0xFFFFFFFFu };
        std::vector<float> Packed;
        CHECK(anari_usd_rawgeo::PackParticleIds(Ids, UsdBridgeType::UINT, 4, 1, Packed),
            "UINT packing succeeds");
        CHECK(Packed.size() == 8, "UINT emits two chunks per ID");
        for (uint32_t i = 0; i < 4; ++i)
        {
            CHECK(Decode(Packed, i * 2, 2) == Ids[i], "UINT ID round-trips exactly");
        }
    }

    {
        const uint64_t Ids[] = {
            0ull,
            0xFFFFull,
            0x00000000FFFFFFFFull,
            0x0123456789ABCDEFull,
            0xFFFFFFFFFFFFFFFFull,
        };
        std::vector<float> Packed;
        CHECK(anari_usd_rawgeo::PackParticleIds(Ids, UsdBridgeType::ULONG, 5, 1, Packed),
            "ULONG packing succeeds");
        CHECK(Packed.size() == 20, "ULONG emits four chunks per ID");
        for (uint32_t i = 0; i < 5; ++i)
        {
            CHECK(Decode(Packed, i * 4, 4) == Ids[i], "ULONG ID round-trips exactly");
        }
    }

    {
        // Width is a property of the source type, not the value: a 64-bit ID
        // type still reserves four chunks even when every value is tiny.
        const uint64_t Ids[] = { 7ull };
        std::vector<float> Packed;
        CHECK(anari_usd_rawgeo::PackParticleIds(Ids, UsdBridgeType::ULONG, 1, 1, Packed),
            "small ULONG packing succeeds");
        CHECK(Packed.size() == 4, "small ULONG still emits four chunks");
        CHECK(Decode(Packed, 0, 4) == 7ull, "small ULONG round-trips");
    }

    {
        const uint16_t Ids[] = { 12u, 34u, 56u };
        std::vector<float> Packed;
        CHECK(anari_usd_rawgeo::PackParticleIds(Ids, UsdBridgeType::USHORT, 3, 1, Packed),
            "USHORT packing succeeds");
        CHECK(Packed.size() == 3, "USHORT emits one chunk per ID");
        for (uint32_t i = 0; i < 3; ++i)
        {
            CHECK(Decode(Packed, i, 1) == Ids[i], "USHORT ID round-trips");
        }
    }

    {
        const uint32_t Ids[] = { 11u, 22u, 33u, 44u, 55u, 66u };
        std::vector<float> Packed;
        CHECK(anari_usd_rawgeo::PackParticleIds(Ids, UsdBridgeType::UINT, 6, 2, Packed),
            "strided UINT packing succeeds");
        CHECK(Packed.size() == 6, "strided UINT keeps two chunks for three output IDs");
        CHECK(Decode(Packed, 0, 2) == 11u, "stride keeps first ID");
        CHECK(Decode(Packed, 2, 2) == 33u, "stride skips every second source ID");
        CHECK(Decode(Packed, 4, 2) == 55u, "stride keeps third selected ID");
    }

    {
        std::vector<float> Packed;
        const float NotIds[] = { 1.0f, 2.0f, 3.0f };
        CHECK(!anari_usd_rawgeo::PackParticleIds(NotIds, UsdBridgeType::FLOAT3, 1, 1, Packed),
            "unsupported attribute type is rejected");
        CHECK(Packed.empty(), "rejected packing leaves output empty");
    }

    std::printf("OK rawgeo_particle_id\n");
    return 0;
}
