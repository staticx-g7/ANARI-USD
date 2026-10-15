// Copyright 2021 NVIDIA Corporation


#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <thread>
#include <chrono>
#include <array>
#include <mpi.h>

#define ANARI_EXTENSION_UTILITY_IMPL
#include "anari/anari_cpp.hpp"
#include "anari/anari_cpp/ext/std.h"

using uvec2 = std::array<unsigned int, 2>;
using ivec3 = std::array<int, 3>;
using vec3 = std::array<float, 3>;
using vec4 = std::array<float, 4>;
using box3 = std::array<vec3, 2>;

ANARIStatusSeverity g_minSeverity = ANARI_SEVERITY_WARNING;

void statusFunc(const void *userData,
    ANARIDevice device,
    ANARIObject source,
    ANARIDataType sourceType,
    ANARIStatusSeverity severity,
    ANARIStatusCode code,
    const char *message)
{
  (void)userData;
  (void)device;
  (void)source;
  (void)sourceType;
  (void)code;
  if (severity > g_minSeverity)
    return;
  if (severity == ANARI_SEVERITY_FATAL_ERROR)
    fprintf(stderr, "[FATAL] %s\n", message);
  else if (severity == ANARI_SEVERITY_ERROR)
    fprintf(stderr, "[ERROR] %s\n", message);
  else if (severity == ANARI_SEVERITY_WARNING)
    fprintf(stderr, "[WARN ] %s\n", message);
  else if (severity == ANARI_SEVERITY_PERFORMANCE_WARNING)
    fprintf(stderr, "[PERF ] %s\n", message);
  else if (severity == ANARI_SEVERITY_INFO)
    fprintf(stderr, "[INFO ] %s\n", message);
  else if (severity == ANARI_SEVERITY_DEBUG)
    fprintf(stderr, "[DEBUG] %s\n", message);
}

int main(int argc, char **argv)
{
  MPI_Init(&argc, &argv);

  for (int i = 1; i < argc; ++i)
  {
    if (strcmp(argv[i], "-v") == 0 && i + 1 < argc)
    {
      int v = atoi(argv[++i]);
      if (v >= 0 && v <= 5)
        g_minSeverity = (ANARIStatusSeverity)v;
    }
  }

  int rank, size;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  printf("[rank %d/%d] Starting MPI USD tutorial\n", rank, size);

  uvec2 imgSize = {1024, 768};

  vec3 cam_pos = {14, 14, 14};
  vec3 cam_up = {0.f, 1.f, 0.f};
  vec3 cam_view = {-1.0f, -1.3f, -1.0f};

  float xOffset = rank * 3.0f;

  vec3 vertex[] = {
    {-1.0f + xOffset, -1.0f, 3.0f},
    {-1.0f + xOffset,  1.0f, 3.0f},
    { 1.0f + xOffset, -1.0f, 3.0f},
    { 0.1f + xOffset,  0.1f, 0.3f}
  };

  float r = (rank % 3 == 0) ? 0.9f : 0.2f;
  float g = (rank % 3 == 1) ? 0.9f : 0.2f;
  float b = (rank % 3 == 2) ? 0.9f : 0.2f;

  vec4 color[] = {
    {r, g, b, 1.0f},
    {0.8f, 0.8f, 0.8f, 1.0f},
    {0.8f, 0.8f, 0.8f, 1.0f},
    {g, b, r, 1.0f}
  };

  ivec3 index[] = {{0, 1, 2}, {1, 2, 3}};


  anari::Library lib = anari::loadLibrary("usd", statusFunc);

  anari::Extensions extensions =
    anari::extension::getDeviceExtensionStruct(lib, "default");

  if (!extensions.ANARI_KHR_DATA_PARALLEL_MPI)
    printf("[rank %d] WARNING: device doesn't report ANARI_KHR_DATA_PARALLEL_MPI\n", rank);

  ANARIDevice d = anariNewDevice(lib, "default");

  anari::setParameter(d, d, "usd::serialize.location", "./mpi_output");

  MPI_Comm comm = MPI_COMM_WORLD;
  anariSetParameter(d, d, "mpiCommunicator", ANARI_VOID_POINTER, &comm);

  anari::commitParameters(d, d);

  printf("[rank %d] Device committed, setting up scene...\n", rank);

  auto camera = anari::newObject<anari::Camera>(d, "perspective");
  anari::setParameter(d, camera, "aspect", (float)imgSize[0] / (float)imgSize[1]);
  anari::setParameter(d, camera, "position", cam_pos);
  anari::setParameter(d, camera, "direction", cam_view);
  anari::setParameter(d, camera, "up", cam_up);
  anari::commitParameters(d, camera);

  auto world = anari::newObject<anari::World>(d);

  auto mesh = anari::newObject<anari::Geometry>(d, "triangle");
  anari::setAndReleaseParameter(
      d, mesh, "vertex.position", anari::newArray1D(d, vertex, 4));
  anari::setAndReleaseParameter(
      d, mesh, "vertex.color", anari::newArray1D(d, color, 4));
  anari::setAndReleaseParameter(
      d, mesh, "primitive.index", anari::newArray1D(d, index, 2));
  anari::commitParameters(d, mesh);

  auto mat = anari::newObject<anari::Material>(d, "matte");
  anari::setParameter(d, mat, "color", "color");
  anari::commitParameters(d, mat);

  auto surface = anari::newObject<anari::Surface>(d);
  anari::setAndReleaseParameter(d, surface, "geometry", mesh);
  anari::setAndReleaseParameter(d, surface, "material", mat);
  anari::commitParameters(d, surface);

  anari::setAndReleaseParameter(
      d, world, "surface", anari::newArray1D(d, &surface));
  anari::release(d, surface);

  auto light = anari::newObject<anari::Light>(d, "directional");
  anari::setParameter(d, light, "irradiance", 1.0f);
  anari::commitParameters(d, light);
  anari::setAndReleaseParameter(
      d, world, "light", anari::newArray1D(d, &light));
  anari::release(d, light);

  anari::commitParameters(d, world);

  auto renderer = anari::newObject<anari::Renderer>(d, "hydra");
  anari::setParameter(d, renderer, "name", "MainRenderer");
  vec4 bgColor = {1.f, 1.f, 1.f, 1.f};
  anari::setParameter(d, renderer, "backgroundColor", bgColor);
  anari::commitParameters(d, renderer);

  auto frame = anari::newObject<anari::Frame>(d);
  anari::setParameter(d, frame, "size", imgSize);
  anari::setParameter(d, frame, "channel.color", ANARI_UFIXED8_RGBA_SRGB);
  anari::setAndReleaseParameter(d, frame, "renderer", renderer);
  anari::setParameter(d, frame, "camera", camera); // kept alive for the anim loop below
  anari::setAndReleaseParameter(d, frame, "world", world);
  anari::commitParameters(d, frame);

  printf("[rank %d] Rendering...\n", rank);

  for (int i = 0; i < 12; ++i) {
    const float a = 0.35f * float(i);
    vec3 anim_pos = {20.f * cosf(a), 20.f * sinf(a), 8.f};
    anari::setParameter(d, camera, "position", anim_pos);
    anari::commitParameters(d, camera);
    anari::render(d, frame);
    anari::wait(d, frame);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  printf("[rank %d] Done rendering, cleaning up...\n", rank);
  anari::release(d, camera);

  anari::release(d, frame);
  anari::release(d, d);
  anari::unloadLibrary(lib);

  MPI_Finalize();

  printf("[rank %d] Finished.\n", rank);
  return 0;
}
