// Host test for the sender-side RawGeo v3 curve packer. Mirrors the CHECK/main
// style of the existing protocol tests; no UE, ANARI or USD dependencies.

#include "UsdBridge/RawGeoCurvePack.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace proto = anari_usd_protocol;

static int g_fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fails; std::printf("FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

namespace
{
struct Fixture
{
    std::vector<int32_t> Lengths;
    std::vector<float> Points;
    std::vector<float> Colors;
    std::vector<float> Widths;
    jusync_rawgeo::CurvePackInput Input;
};

void MakeFixture(Fixture& F, const std::vector<int32_t>& Lengths,
                 uint64_t SubsetIndex, uint64_t SubsetCount, uint64_t FullCurveCount)
{
    F.Lengths = Lengths;

    int32_t Total = 0;
    for (const int32_t Length : F.Lengths)
    {
        Total += Length;
    }
    F.Points.assign(static_cast<size_t>(Total) * 3, 0.0f);
    F.Colors.assign(static_cast<size_t>(Total) * 3, 1.0f);
    F.Widths.assign(static_cast<size_t>(Total), 0.1f);
    for (int32_t i = 0; i < Total; ++i)
    {
        const size_t Base = static_cast<size_t>(i) * 3;
        F.Points[Base + 0] = static_cast<float>(i) + static_cast<float>(SubsetIndex) * 100.0f;
        F.Points[Base + 1] = 0.5f;
        F.Points[Base + 2] = 1.0f;
        F.Colors[Base + 1] = 0.5f;
        F.Colors[Base + 2] = 0.25f;
    }

    F.Input = jusync_rawgeo::CurvePackInput{};
    F.Input.PrimName = "Streamlines";
    F.Input.Time = 1.5;
    F.Input.Points = F.Points.data();
    F.Input.NumPoints = static_cast<uint64_t>(Total);
    F.Input.Colors = F.Colors.data();
    F.Input.ColorComponents = 3;
    F.Input.Widths = F.Widths.data();
    F.Input.CurveLengths = F.Lengths.data();
    F.Input.NumCurves = F.Lengths.size();
    F.Input.Attributes = nullptr;
    F.Input.NumAttributes = 0;
    F.Input.SubsetIndex = SubsetIndex;
    F.Input.SubsetCount = SubsetCount;
    F.Input.FullCurveCount = FullCurveCount;
    F.Input.FullNumPoints = 0;
}

bool Pack(const Fixture& F, std::vector<uint8_t>& Out, proto::JusyncRawGeoHeader& Header)
{
    if (!jusync_rawgeo::PackCurvePayload(F.Input, Out))
    {
        return false;
    }
    if (Out.size() < sizeof(Header))
    {
        return false;
    }
    std::memcpy(&Header, Out.data(), sizeof(Header));
    return true;
}

std::vector<uint32_t> ReadCounts(const proto::JusyncRawGeoHeader& Header,
                                 const std::vector<uint8_t>& Out)
{
    uint32_t AttrComponents = 0;
    const auto* Descs = reinterpret_cast<const proto::JusyncRawGeoAttrDesc*>(
        Out.data() + sizeof(Header));
    for (uint32_t a = 0; a < Header.num_attributes; ++a)
    {
        AttrComponents += Descs[a].components;
    }

    const size_t Offset = sizeof(Header)
        + Header.num_attributes * sizeof(proto::JusyncRawGeoAttrDesc)
        + Header.num_points * 3 * sizeof(float)
        + Header.num_points * Header.color_components * sizeof(float)
        + Header.num_points * AttrComponents * sizeof(float);
    std::vector<uint32_t> Counts(Header.curve_count);
    if (Header.curve_count > 0)
    {
        std::memcpy(Counts.data(), Out.data() + Offset, Header.curve_count * sizeof(uint32_t));
    }
    return Counts;
}

bool HasKey(const std::vector<std::string>& Keys, const std::string& Key)
{
    for (const std::string& Candidate : Keys)
    {
        if (Candidate == Key)
        {
            return true;
        }
    }
    return false;
}

bool Rejects(Fixture& F)
{
    std::vector<uint8_t> Out;
    return !jusync_rawgeo::PackCurvePayload(F.Input, Out);
}
}

int main()
{
    CHECK(sizeof(proto::JusyncRawGeoHeader) == 160, "v3 header size");

    {
        Fixture F;
        MakeFixture(F, { 3, 2, 4 }, 0, 1, 3);

        std::vector<uint8_t> Out;
        proto::JusyncRawGeoHeader Header{};
        CHECK(Pack(F, Out, Header), "S=1 packs");
        CHECK(Header.valid(Out.data(), Out.size()), "S=1 valid");
        CHECK(Header.num_points == 9, "S=1 num_points");
        CHECK(Header.curve_count == 3, "S=1 curve_count");
        CHECK(Header.full_curve_count == 3, "S=1 full_curve_count");
        CHECK(Header.width_components == 1, "S=1 widths");
        CHECK(Header.attr_uniform_count == 9, "S=1 attr_uniform_count");
        CHECK(Header.payload_bytes == 9 * 12 + 9 * 3 * 4 + 3 * 4 + 9 * 4,
            "S=1 payload_bytes (%llu)", static_cast<unsigned long long>(Header.payload_bytes));
        const std::vector<uint32_t> Counts = ReadCounts(Header, Out);
        CHECK(Counts.size() == 3 && Counts[0] == 3 && Counts[1] == 2 && Counts[2] == 4,
            "S=1 curve lengths round-trip");
    }

    {
        Fixture F0;
        MakeFixture(F0, { 3, 4 }, 0, 2, 3);
        std::vector<uint8_t> Out0;
        proto::JusyncRawGeoHeader Header0{};
        CHECK(Pack(F0, Out0, Header0), "S=2 subset 0 packs");
        CHECK(Header0.valid(Out0.data(), Out0.size()), "S=2 subset 0 valid");
        CHECK(Header0.num_points == 7, "S=2 subset 0 points");
        CHECK(Header0.curve_count == 2, "S=2 subset 0 curves");
        CHECK(Header0.full_curve_count == 3, "S=2 subset 0 full curves");
        const std::vector<uint32_t> Counts0 = ReadCounts(Header0, Out0);
        CHECK(Counts0.size() == 2 && Counts0[0] == 3 && Counts0[1] == 4,
            "S=2 subset 0 ordinals 0,2");

        Fixture F1;
        MakeFixture(F1, { 2 }, 1, 2, 3);
        std::vector<uint8_t> Out1;
        proto::JusyncRawGeoHeader Header1{};
        CHECK(Pack(F1, Out1, Header1), "S=2 subset 1 packs");
        CHECK(Header1.valid(Out1.data(), Out1.size()), "S=2 subset 1 valid");
        CHECK(Header1.num_points == 2, "S=2 subset 1 points");
        CHECK(Header1.curve_count == 1, "S=2 subset 1 curves");
        const std::vector<uint32_t> Counts1 = ReadCounts(Header1, Out1);
        CHECK(Counts1.size() == 1 && Counts1[0] == 2, "S=2 subset 1 ordinal 1");
    }

    for (uint64_t Subset = 1; Subset < 4; ++Subset)
    {
        Fixture F;
        MakeFixture(F, {}, Subset, 4, 1);
        std::vector<uint8_t> Out;
        proto::JusyncRawGeoHeader Header{};
        CHECK(Pack(F, Out, Header), "S=4 empty subset %llu packs",
            static_cast<unsigned long long>(Subset));
        CHECK(Header.valid(Out.data(), Out.size()), "S=4 empty subset %llu valid",
            static_cast<unsigned long long>(Subset));
        CHECK(Header.num_points == 0 && Header.curve_count == 0 && Header.payload_bytes == 0,
            "S=4 empty subset %llu is header-only", static_cast<unsigned long long>(Subset));
    }

    {
        Fixture F;
        MakeFixture(F, { 3, 2, 4 }, 0, 1, 3);
        F.Lengths[0] = 1;
        F.Input.CurveLengths = F.Lengths.data();
        CHECK(Rejects(F), "length < 2 rejected");
    }
    {
        Fixture F;
        MakeFixture(F, { 3, 2, 4 }, 0, 1, 3);
        F.Input.NumPoints += 1;
        CHECK(Rejects(F), "point sum mismatch rejected");
    }
    {
        Fixture F;
        MakeFixture(F, { 3, 2, 4 }, 0, 1, 3);
        F.Input.SubsetCount = 0;
        CHECK(Rejects(F), "subset_count 0 rejected");
    }
    {
        Fixture F;
        MakeFixture(F, { 3, 2, 4 }, 0, 1, 3);
        F.Input.SubsetIndex = 1;
        F.Input.SubsetCount = 1;
        CHECK(Rejects(F), "subset_index >= subset_count rejected");
    }
    {
        Fixture F;
        MakeFixture(F, { 3, 2, 4 }, 0, 1, 3);
        F.Input.ColorComponents = 2;
        CHECK(Rejects(F), "color components 2 rejected");
    }
    {
        Fixture F;
        MakeFixture(F, { 3, 2, 4 }, 0, 1, 3);
        F.Input.NumAttributes = 17;
        CHECK(Rejects(F), "more than 16 attributes rejected");
    }

    CHECK(jusync_rawgeo::AddSubsetSuffix("a.geo", 3, 4) == "a_s3.geo", "subset suffix inserted");
    CHECK(jusync_rawgeo::AddSubsetSuffix("a.geo", 0, 1) == "a.geo", "S=1 keeps the name");

    // C=10, S=4: ordinals 0+4+8, 1+5+9, 2+6, 3+7 -> 3,3,2,2.
    {
        const int32_t Expected[4] = { 3, 3, 2, 2 };
        for (uint64_t Subset = 0; Subset < 4; ++Subset)
        {
            const std::vector<int32_t> Lengths(static_cast<size_t>(Expected[Subset]), 2);
            Fixture F;
            MakeFixture(F, Lengths, Subset, 4, 10);
            std::vector<uint8_t> Out;
            proto::JusyncRawGeoHeader Header{};
            CHECK(Pack(F, Out, Header), "C=10 S=4 subset %llu packs",
                static_cast<unsigned long long>(Subset));
            CHECK(Header.valid(Out.data(), Out.size()), "C=10 S=4 subset %llu valid",
                static_cast<unsigned long long>(Subset));
            CHECK(Header.curve_count == static_cast<uint32_t>(Expected[Subset]),
                "C=10 S=4 subset %llu curve count %u",
                static_cast<unsigned long long>(Subset), Header.curve_count);
            CHECK(Header.full_curve_count == 10, "C=10 S=4 full curve count");
            CHECK(Header.num_points == static_cast<uint64_t>(Expected[Subset] * 2),
                "C=10 S=4 subset %llu point count",
                static_cast<unsigned long long>(Subset));
        }
    }
    {
        Fixture F;
        MakeFixture(F, std::vector<int32_t>(4, 2), 0, 4, 10);
        CHECK(Rejects(F), "C=10 S=4 wrong subset count rejected");
    }

    // Two attributes must round-trip after colors and before curve lengths.
    {
        Fixture F;
        MakeFixture(F, { 3, 2, 4 }, 0, 1, 3);
        std::vector<float> Temperature(9 * 2);
        std::vector<float> Pressure(9 * 3);
        for (size_t i = 0; i < Temperature.size(); ++i)
        {
            Temperature[i] = static_cast<float>(i) + 0.25f;
        }
        for (size_t i = 0; i < Pressure.size(); ++i)
        {
            Pressure[i] = static_cast<float>(i) + 0.5f;
        }
        jusync_rawgeo::CurveAttribute Attrs[2] = {
            { "temperature", 2, Temperature.data() },
            { "pressure", 3, Pressure.data() },
        };
        F.Input.Attributes = Attrs;
        F.Input.NumAttributes = 2;

        std::vector<uint8_t> Out;
        proto::JusyncRawGeoHeader Header{};
        CHECK(Pack(F, Out, Header), "attributes pack");
        CHECK(Header.valid(Out.data(), Out.size()), "attributes valid");
        CHECK(Header.num_attributes == 2, "two attribute descriptors");
        CHECK(Header.payload_bytes ==
            9 * 12 + 9 * 3 * 4 + 9 * 2 * 4 + 9 * 3 * 4 + 3 * 4 + 9 * 4,
            "attribute payload bytes");

        const size_t AttrOffset = sizeof(Header)
            + 2 * sizeof(proto::JusyncRawGeoAttrDesc)
            + 9 * 3 * sizeof(float)
            + 9 * 3 * sizeof(float);
        CHECK(std::memcmp(Out.data() + AttrOffset, Temperature.data(),
            Temperature.size() * sizeof(float)) == 0, "temperature round-trip");
        CHECK(std::memcmp(Out.data() + AttrOffset + Temperature.size() * sizeof(float),
            Pressure.data(), Pressure.size() * sizeof(float)) == 0, "pressure round-trip");

        const std::vector<uint32_t> Counts = ReadCounts(Header, Out);
        CHECK(Counts.size() == 3 && Counts[0] == 3 && Counts[1] == 2 && Counts[2] == 4,
            "curve lengths follow attributes");
    }

    // Removing a retired clip must invalidate every subset alias the curve
    // writer may have produced for that base key.
    {
        const std::string Base = "clips/Wave_Geom__0.000000";
        const std::vector<std::string> Keys =
            jusync_rawgeo::StoreKeysForFile(Base + ".usda");
        CHECK(HasKey(Keys, Base + ".usda"), "store keys keep the input");
        CHECK(HasKey(Keys, Base + ".usd"), "store keys add the USD twin");
        CHECK(HasKey(Keys, Base + ".geo"), "store keys add the base geometry");
        for (uint64_t Subset = 0; Subset < 8; ++Subset)
        {
            CHECK(HasKey(Keys, Base + "_s" + std::to_string(Subset) + ".geo"),
                "store keys add subset %llu",
                static_cast<unsigned long long>(Subset));
        }
        CHECK(Keys.size() == 11, "store keys deduplicate (%zu)", Keys.size());
    }
    {
        const std::string Base = "clips/Wave_Geom__0.000000";
        const std::vector<std::string> Keys =
            jusync_rawgeo::StoreKeysForFile(Base + "_s3.geo");
        CHECK(HasKey(Keys, Base + ".geo"), "subset input adds the base geometry");
        CHECK(HasKey(Keys, Base + "_s0.geo"), "subset input adds sibling zero");
        CHECK(HasKey(Keys, Base + "_s3.geo"), "subset input keeps itself");
        CHECK(!HasKey(Keys, Base + "_s3_s3.geo"), "subset input is not double-suffixed");
        CHECK(Keys.size() == 11, "subset input deduplicates (%zu)", Keys.size());
    }
    {
        const std::vector<std::string> Keys =
            jusync_rawgeo::StoreKeysForFile("images/frame.png");
        CHECK(Keys.size() == 1 && Keys[0] == "images/frame.png",
            "non-stage files keep one store key");
    }

    if (g_fails)
    {
        std::printf("FAILED %d\n", g_fails);
        return 1;
    }
    std::printf("OK rawgeo_curve_pack\n");
    return 0;
}
