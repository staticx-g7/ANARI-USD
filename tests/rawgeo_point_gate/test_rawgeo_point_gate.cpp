// Host test for the RawGeo point-stream gate. RawGeo v2 carries no topology,
// so only sphere point clouds may be flattened to points.

#include "UsdBridge/Common/UsdBridgeData.h"

#include <cstdio>

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); return 1; } } while (0)

int main()
{
    float points[4 * 3] = {};

    UsdBridgeInstancerData cloud;
    cloud.IsPointCloud = true;
    cloud.Points = points;
    cloud.NumPoints = 4;
    cloud.PointsType = UsdBridgeType::FLOAT3;
    CHECK(UsdBridgeCanStreamPointCloud(cloud), "sphere point cloud is eligible");

    UsdBridgeInstancerData cylinder = cloud;
    cylinder.IsPointCloud = false;
    CHECK(!UsdBridgeCanStreamPointCloud(cylinder),
        "cylinder/cone/glyph surface instancer is not flattened to points");

    UsdBridgeInstancerData empty;
    empty.IsPointCloud = true;
    empty.NumPoints = 0;
    CHECK(!UsdBridgeCanStreamPointCloud(empty), "empty point cloud is rejected");

    UsdBridgeInstancerData doublePoints = cloud;
    doublePoints.PointsType = UsdBridgeType::DOUBLE3;
    CHECK(!UsdBridgeCanStreamPointCloud(doublePoints),
        "non-float3 point cloud is rejected");

    std::printf("OK rawgeo_point_gate\n");
    return 0;
}
