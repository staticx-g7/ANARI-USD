// Copyright 2026 NVIDIA Corporation
// SPDX-License-Identifier: Apache-2.0
//
// Creates two ANARI worlds (each with geometry + material) and two frames.
// Rendering each frame emits Session_*/frame_*.usda entry stages that reference
// only that frame's world, camera, and RenderContext from FullScene.usda.

#include <array>
#include <cstdio>
#include <cstring>

#define ANARI_EXTENSION_UTILITY_IMPL
#include "anari/anari_cpp.hpp"
#include "anari/anari_cpp/ext/std.h"

using uvec2 = std::array<unsigned int, 2>;
using ivec3 = std::array<int, 3>;
using vec3 = std::array<float, 3>;
using vec4 = std::array<float, 4>;

ANARIStatusSeverity g_minSeverity = ANARI_SEVERITY_WARNING;

void statusFunc(const void * /*userData*/,
    ANARIDevice /*device*/,
    ANARIObject /*source*/,
    ANARIDataType /*sourceType*/,
    ANARIStatusSeverity severity,
    ANARIStatusCode /*code*/,
    const char *message)
{
  if (severity > g_minSeverity)
    return;
  if (severity == ANARI_SEVERITY_FATAL_ERROR) {
    fprintf(stderr, "[FATAL] %s\n", message);
  } else if (severity == ANARI_SEVERITY_ERROR) {
    fprintf(stderr, "[ERROR] %s\n", message);
  } else if (severity == ANARI_SEVERITY_WARNING) {
    fprintf(stderr, "[WARN ] %s\n", message);
  } else if (severity == ANARI_SEVERITY_PERFORMANCE_WARNING) {
    fprintf(stderr, "[PERF ] %s\n", message);
  } else if (severity == ANARI_SEVERITY_INFO) {
    fprintf(stderr, "[INFO ] %s\n", message);
  } else if (severity == ANARI_SEVERITY_DEBUG) {
    fprintf(stderr, "[DEBUG] %s\n", message);
  }
}

struct WorldBundle
{
  anari::World world;
  anari::Camera camera;
  anari::Frame frame;
};

static anari::Surface makeColoredTriangleSurface(anari::Device d,
    const char *geomName,
    const char *matName,
    const char *surfaceName,
    const vec3 &color,
    const vec3 &offset)
{
  vec3 vertex[] = {{offset[0] - 1.0f, offset[1] - 1.0f, offset[2] + 3.0f},
      {offset[0] - 1.0f, offset[1] + 1.0f, offset[2] + 3.0f},
      {offset[0] + 1.0f, offset[1] - 1.0f, offset[2] + 3.0f}};
  ivec3 index[] = {{0, 1, 2}};

  auto mesh = anari::newObject<anari::Geometry>(d, "triangle");
  anari::setParameter(d, mesh, "name", geomName);
  anari::setAndReleaseParameter(
      d, mesh, "vertex.position", anari::newArray1D(d, vertex, 3));
  anari::setAndReleaseParameter(
      d, mesh, "primitive.index", anari::newArray1D(d, index, 1));
  anari::commitParameters(d, mesh);

  auto mat = anari::newObject<anari::Material>(d, "matte");
  anari::setParameter(d, mat, "name", matName);
  anari::setParameter(d, mat, "color", color);
  anari::commitParameters(d, mat);

  auto surface = anari::newObject<anari::Surface>(d);
  anari::setParameter(d, surface, "name", surfaceName);
  anari::setAndReleaseParameter(d, surface, "geometry", mesh);
  anari::setAndReleaseParameter(d, surface, "material", mat);
  anari::commitParameters(d, surface);
  return surface;
}

static WorldBundle makeWorldFrame(anari::Device d,
    const uvec2 &imgSize,
    const char *worldName,
    const char *cameraName,
    const char *frameName,
    const char *geomName,
    const char *matName,
    const char *surfaceName,
    const char *lightName,
    const vec3 &color,
    const vec3 &geomOffset,
    const vec3 &camPos,
    const vec3 &camDir)
{
  WorldBundle out{};

  out.camera = anari::newObject<anari::Camera>(d, "perspective");
  anari::setParameter(d, out.camera, "name", cameraName);
  anari::setParameter(
      d, out.camera, "aspect", (float)imgSize[0] / (float)imgSize[1]);
  anari::setParameter(d, out.camera, "position", camPos);
  anari::setParameter(d, out.camera, "direction", camDir);
  anari::setParameter(d, out.camera, "up", vec3{0.f, 1.f, 0.f});
  anari::commitParameters(d, out.camera);

  out.world = anari::newObject<anari::World>(d);
  anari::setParameter(d, out.world, "name", worldName);

  auto surface = makeColoredTriangleSurface(
      d, geomName, matName, surfaceName, color, geomOffset);
  anari::setAndReleaseParameter(
      d, out.world, "surface", anari::newArray1D(d, &surface));
  anari::release(d, surface);

  auto light = anari::newObject<anari::Light>(d, "directional");
  anari::setParameter(d, light, "name", lightName);
  anari::setParameter(d, light, "irradiance", 1.0f);
  anari::commitParameters(d, light);
  anari::setAndReleaseParameter(
      d, out.world, "light", anari::newArray1D(d, &light));
  anari::release(d, light);

  anari::commitParameters(d, out.world);

  auto renderer = anari::newObject<anari::Renderer>(d, "hydra");
  anari::setParameter(d, renderer, "name", frameName);
  anari::setParameter(d, renderer, "backgroundColor", vec4{1.f, 1.f, 1.f, 1.f});
  anari::commitParameters(d, renderer);

  out.frame = anari::newObject<anari::Frame>(d);
  anari::setParameter(d, out.frame, "name", frameName);
  anari::setParameter(d, out.frame, "size", imgSize);
  anari::setParameter(d, out.frame, "channel.color", ANARI_UFIXED8_RGBA_SRGB);
  anari::setAndReleaseParameter(d, out.frame, "renderer", renderer);
  anari::setParameter(d, out.frame, "camera", out.camera);
  anari::setParameter(d, out.frame, "world", out.world);
  anari::commitParameters(d, out.frame);

  return out;
}

int main(int argc, const char **argv)
{
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-v") == 0 && i + 1 < argc) {
      int v = atoi(argv[++i]);
      if (v >= 0 && v <= 5)
        g_minSeverity = (ANARIStatusSeverity)v;
    }
  }

  printf("initialize ANARI...");
  anari::Library lib = anari::loadLibrary("usd", statusFunc);
  ANARIDevice d = anariNewDevice(lib, "default");
  printf("done!\n");

  uvec2 imgSize = {1024, 768};

  // Aim each camera at its own triangle (center ~ offset + (0,0,3)).
  printf("building world_red / frame_red...");
  WorldBundle red = makeWorldFrame(d,
      imgSize,
      "world_red",
      "camera_red",
      "frame_red",
      "geom_red",
      "mat_red",
      "surface_red",
      "light_red",
      vec3{0.9f, 0.1f, 0.1f},
      vec3{-1.5f, 0.f, 0.f},
      vec3{-1.5f, 2.0f, 8.0f},
      vec3{0.0f, -0.25f, -1.0f});
  printf("done!\n");

  printf("building world_blue / frame_blue...");
  WorldBundle blue = makeWorldFrame(d,
      imgSize,
      "world_blue",
      "camera_blue",
      "frame_blue",
      "geom_blue",
      "mat_blue",
      "surface_blue",
      "light_blue",
      vec3{0.1f, 0.3f, 0.9f},
      vec3{1.5f, 0.f, 0.f},
      vec3{1.5f, 2.0f, 8.0f},
      vec3{0.0f, -0.25f, -1.0f});
  printf("done!\n");

  printf("rendering frame_red (emits frame_red.usda)...");
  anari::render(d, red.frame);
  anari::wait(d, red.frame);
  printf("done!\n");

  printf("rendering frame_blue (emits frame_blue.usda)...");
  anari::render(d, blue.frame);
  anari::wait(d, blue.frame);
  printf("done!\n");

  printf("cleaning up...");
  anari::release(d, red.frame);
  anari::release(d, red.camera);
  anari::release(d, red.world);
  anari::release(d, blue.frame);
  anari::release(d, blue.camera);
  anari::release(d, blue.world);
  anari::release(d, d);
  anari::unloadLibrary(lib);
  printf("done!\n");

  return 0;
}
