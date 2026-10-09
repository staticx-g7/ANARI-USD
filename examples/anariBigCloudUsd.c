// Copyright 2026 JUSYNC contributors
// SPDX-License-Identifier: Apache-2.0

// Scalable point-cloud stress sender for the JUSYNC streaming pipeline.
//
// Publishes ONE large point cloud ("bigCloud", an ANARI "sphere" geometry
// without useUsdGeomPoints, i.e. written as UsdGeomPointInstancer) and
// re-commits it once per frame at a FIXED usd::time, so each streamed clip
// update carries exactly one timeSample of the current positions (history
// never accumulates inside a clip file).
//
// No renderer is attached: UsdDevice::renderFrame performs the USD save and
// the ZMQ file notifications before the actual (optional) hydra draw, so the
// full export/streaming path runs while rendering stays free.
//
// Environment knobs:
//   ANARI_BIG_POINTS   points per cloud   (default 10000000)
//   ANARI_BIG_FPS      commits per second (default 2)
//   ANARI_BIG_SECONDS  run duration, 0 = forever (default 0)
//   ANARI_BIG_COLORS   1 = add per-vertex float3 colors (default 0)
//   ANARI_BIG_RADIUS   constant point radius (default 1.0)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "anari/anari.h"

static ANARIStatusSeverity g_minSeverity = ANARI_SEVERITY_WARNING;

static void statusFunc(const void *userData,
    ANARIDevice device,
    ANARIObject source,
    ANARIDataType sourceType,
    ANARIStatusSeverity severity,
    ANARIStatusCode code,
    const char *message)
{
  (void)userData; (void)device; (void)source; (void)sourceType; (void)code;
  if (severity > g_minSeverity)
    return;
  const char *tag =
    severity == ANARI_SEVERITY_FATAL_ERROR ? "FATAL" :
    severity == ANARI_SEVERITY_ERROR ? "ERROR" :
    severity == ANARI_SEVERITY_WARNING ? "WARN " :
    severity == ANARI_SEVERITY_PERFORMANCE_WARNING ? "PERF " :
    severity == ANARI_SEVERITY_INFO ? "INFO " : "DEBUG";
  fprintf(stderr, "[%s] %s\n", tag, message);
}

static void parseArgs(int argc, const char **argv)
{
  for (int i = 1; i < argc; ++i)
  {
    if (strcmp(argv[i], "-v") == 0 && i + 1 < argc)
    {
      int v = atoi(argv[++i]);
      if (v >= 0 && v <= 5)
        g_minSeverity = (ANARIStatusSeverity)v;
    }
  }
}

static double nowMs(void)
{
#ifdef _WIN32
  return GetTickCount64();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6;
#endif
}

static void sleepMs(long ms)
{
#ifdef _WIN32
  Sleep((DWORD)ms);
#else
  nanosleep(&(struct timespec){ms / 1000, (ms % 1000) * 1000000L}, NULL);
#endif
}

static long envLong(const char *name, long dflt)
{
  const char *v = getenv(name);
  return v ? atol(v) : dflt;
}

int main(int argc, const char **argv)
{
  parseArgs(argc, argv);

  const long numPoints = envLong("ANARI_BIG_POINTS", 10000000L);
  const long fps = envLong("ANARI_BIG_FPS", 2L);
  const long seconds = envLong("ANARI_BIG_SECONDS", 0L);
  const int useColors = envLong("ANARI_BIG_COLORS", 0L) != 0;
  float radius = (float)envLong("ANARI_BIG_RADIUS", 1L);

  if (numPoints < 1) {
    fprintf(stderr, "ANARI_BIG_POINTS must be >= 1\n");
    return 1;
  }

  printf("bigcloud: %ld points, %ld fps, %s, colors=%d\n",
      numPoints, fps, seconds > 0 ? "limited" : "infinite", useColors);
  fflush(stdout);

  // --- host-side buffers -------------------------------------------------
  float *base = (float *)malloc((size_t)numPoints * 3 * sizeof(float));
  float *positions = (float *)malloc((size_t)numPoints * 3 * sizeof(float));
  float *colors = NULL;
  if (!base || !positions) {
    fprintf(stderr, "bigcloud: out of memory for %ld points\n", numPoints);
    return 1;
  }
  if (useColors)
    colors = (float *)malloc((size_t)numPoints * 3 * sizeof(float));

  srand(42);
  for (long i = 0; i < numPoints * 3; ++i)
    base[i] = ((rand() / (float)RAND_MAX) * 2.0f - 1.0f) * 100.0f;
  for (long i = 0; i < numPoints * 3; ++i)
    positions[i] = base[i];
  if (colors) {
    for (long i = 0; i < numPoints; ++i) {
      colors[i * 3 + 0] = (float)((i * 2654435761u >> 16) & 0xFF) / 255.0f;
      colors[i * 3 + 1] = (float)((i * 40503u >> 8) & 0xFF) / 255.0f;
      colors[i * 3 + 2] = 0.75f;
    }
  }

  // --- ANARI device ------------------------------------------------------
  ANARILibrary lib = anariLoadLibrary("usd", statusFunc, NULL);
  ANARIDevice dev = anariNewDevice(lib, "usd");
  if (!dev) {
    fprintf(stderr, "bigcloud: could not create usd device\n");
    return 1;
  }

  int outputBinary = 1; // crate output: smaller on the wire, faster client parse
  int connLogVerbosity = 0;
  anariSetParameter(dev, dev, "usd::connection.logVerbosity", ANARI_INT32, &connLogVerbosity);
  anariSetParameter(dev, dev, "usd::serialize.outputBinary", ANARI_BOOL, &outputBinary);
  anariCommitParameters(dev, dev);

  // camera + fixed timestep: every commit replaces the t=0 sample
  float cam_pos[] = {0.f, 0.f, 400.f};
  float cam_view[] = {0.f, 0.f, -1.f};
  float cam_up[] = {0.f, 1.f, 0.f};
  ANARICamera camera = anariNewCamera(dev, "perspective");
  float aspect = 1.0f;
  anariSetParameter(dev, camera, "aspect", ANARI_FLOAT32, &aspect);
  anariSetParameter(dev, camera, "position", ANARI_FLOAT32_VEC3, cam_pos);
  anariSetParameter(dev, camera, "direction", ANARI_FLOAT32_VEC3, cam_view);
  anariSetParameter(dev, camera, "up", ANARI_FLOAT32_VEC3, cam_up);
  anariCommitParameters(dev, camera);

  const double timeZero = 0.0;

  const long frameBudgetMs = fps > 0 ? 1000 / fps : 500;
  const double startMs = nowMs();

  ANARIFrame frame = anariNewFrame(dev);
  anariSetParameter(dev, frame, "size", ANARI_UINT32_VEC2, (const uint32_t[]){16, 16});
  ANARIDataType colFormat = ANARI_UFIXED8_RGBA_SRGB;
  anariSetParameter(dev, frame, "channel.color", ANARI_DATA_TYPE, &colFormat);

  // --- build the scene ONCE ----------------------------------------------
  // Handles are retained so every frame re-commits the SAME prim
  // ("bigCloud"): creating fresh ANARI objects per frame would make the
  // bridge uniquify the repeated prim name (bigCloud_u1, ...) and stream
  // duplicate clouds. The UsdDevice memory-store path force re-reads geometry
  // data each flush (see UsdGeometry commitTemplate), so in-place updates of
  // the position buffer are exported without rebuilding the object graph.
  ANARIWorld world = anariNewWorld(dev);

  ANARIGeometry mesh = anariNewGeometry(dev, "sphere");
  anariSetParameter(dev, mesh, "name", ANARI_STRING, "bigCloud");
  int useUsdGeomPoints = 0;
  anariSetParameter(dev, mesh, "usd::useUsdGeomPoints", ANARI_BOOL, &useUsdGeomPoints);
  anariSetParameter(dev, mesh, "radius", ANARI_FLOAT32, &radius);

  ANARIArray1D array = anariNewArray1D(dev, positions, 0, 0, ANARI_FLOAT32_VEC3,
      (uint64_t)numPoints);
  anariCommitParameters(dev, array);
  anariSetParameter(dev, mesh, "vertex.position", ANARI_ARRAY, &array);
  anariRelease(dev, array);

  if (colors) {
    array = anariNewArray1D(dev, colors, 0, 0, ANARI_FLOAT32_VEC3, (uint64_t)numPoints);
    anariCommitParameters(dev, array);
    anariSetParameter(dev, mesh, "vertex.color", ANARI_ARRAY, &array);
    anariRelease(dev, array);
  }

  anariSetParameter(dev, mesh, "usd::time", ANARI_FLOAT64, &timeZero);
  anariCommitParameters(dev, mesh);

  float kd[] = {0.9f, 0.4f, 0.1f};
  ANARIMaterial mat = anariNewMaterial(dev, "matte");
  anariSetParameter(dev, mat, "name", ANARI_STRING, "bigCloudMaterial");
  anariSetParameter(dev, mat, "color", ANARI_FLOAT32_VEC3, kd);
  anariCommitParameters(dev, mat);

  ANARISurface surface = anariNewSurface(dev);
  anariSetParameter(dev, surface, "name", ANARI_STRING, "bigCloudSurface");
  anariSetParameter(dev, surface, "geometry", ANARI_GEOMETRY, &mesh);
  anariSetParameter(dev, surface, "material", ANARI_MATERIAL, &mat);
  anariCommitParameters(dev, surface);

  ANARIGroup group = anariNewGroup(dev);
  anariSetParameter(dev, group, "name", ANARI_STRING, "bigCloudGroup");
  array = anariNewArray1D(dev, &surface, 0, 0, ANARI_SURFACE, 1);
  anariCommitParameters(dev, array);
  anariSetParameter(dev, group, "surface", ANARI_ARRAY, &array);
  anariCommitParameters(dev, group);

  ANARIInstance instance = anariNewInstance(dev, "transform");
  anariSetParameter(dev, instance, "name", ANARI_STRING, "bigCloudInstance");
  float transform[16] = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f };
  anariSetParameter(dev, instance, "transform", ANARI_FLOAT32_MAT4, transform);
  anariSetParameter(dev, instance, "group", ANARI_GROUP, &group);
  anariCommitParameters(dev, instance);

  ANARILight light = anariNewLight(dev, "hdri");
  anariSetParameter(dev, light, "name", ANARI_STRING, "bigCloudLight");
  float lightRadiance = 1.0f;
  anariSetParameter(dev, light, "radiance", ANARI_FLOAT32, &lightRadiance);
  anariCommitParameters(dev, light);

  array = anariNewArray1D(dev, &instance, 0, 0, ANARI_INSTANCE, 1);
  anariCommitParameters(dev, array);
  anariSetParameter(dev, world, "instance", ANARI_ARRAY, &array);
  anariRelease(dev, array);

  array = anariNewArray1D(dev, &light, 0, 0, ANARI_LIGHT, 1);
  anariCommitParameters(dev, array);
  anariSetParameter(dev, world, "light", ANARI_ARRAY, &array);
  anariRelease(dev, array);

  anariSetParameter(dev, world, "name", ANARI_STRING, "bigCloudWorld");
  anariCommitParameters(dev, world);

  // No renderer attached: save + ZMQ notifications still run, hydra draw
  // is skipped inside UsdFrame::renderFrame.
  anariSetParameter(dev, frame, "world", ANARI_WORLD, &world);
  anariSetParameter(dev, frame, "camera", ANARI_CAMERA, &camera);
  anariCommitParameters(dev, frame);

  int frameIdx = 0;
  for (;;)
  {
    const double commitStart = nowMs();

    // Cheap memory-bound animation: uniform scale pulse (real sims change far
    // more of the buffer than this; the streaming cost is byte volume, not
    // which bytes changed).
    const float scale = 1.0f + 0.15f * sinf(frameIdx * 0.35f);
    for (long i = 0; i < numPoints * 3; ++i)
      positions[i] = base[i] * scale;

    // Re-set the position array on the retained mesh: the SDK takes the
    // device-side data snapshot at set/commit of the ARRAY parameter, so
    // in-place edits of the app buffer are invisible unless a fresh array
    // handle is committed (same pattern as the time tutorial, which however
    // rebuilt the whole geometry and thereby churned prim names).
    ANARIArray1D posArray = anariNewArray1D(dev, positions, 0, 0, ANARI_FLOAT32_VEC3,
        (uint64_t)numPoints);
    anariCommitParameters(dev, posArray);
    anariSetParameter(dev, mesh, "vertex.position", ANARI_ARRAY, &posArray);
    anariRelease(dev, posArray);

    anariSetParameter(dev, dev, "usd::time", ANARI_FLOAT64, &timeZero);
    anariCommitParameters(dev, dev);

    // Re-commit the retained objects: this puts them on the device commit
    // list so renderFrame's flush re-reads the position buffer and re-writes
    // the clip (memory-store mode forces paramChanged).
    anariCommitParameters(dev, mesh);
    anariCommitParameters(dev, surface);
    anariCommitParameters(dev, group);
    anariCommitParameters(dev, instance);
    anariCommitParameters(dev, world);

    anariRenderFrame(dev, frame);
    anariFrameReady(dev, frame, ANARI_WAIT);

    const double elapsed = nowMs() - commitStart;
    printf("bigcloud: frame %d commit->flush %.1f ms (%.1f MB positions%s)\n",
        frameIdx, elapsed,
        numPoints * 12.0 / (1024.0 * 1024.0),
        colors ? " + colors" : "");
    fflush(stdout);

    ++frameIdx;

    if (seconds > 0 && (nowMs() - startMs) > seconds * 1000.0)
      break;

    const double spend = nowMs() - commitStart;
    const long wait = frameBudgetMs - (long)spend;
    if (wait > 0)
      sleepMs(wait);
  }

  anariRelease(dev, frame);
  anariRelease(dev, world);
  anariRelease(dev, mesh);
  anariRelease(dev, mat);
  anariRelease(dev, surface);
  anariRelease(dev, group);
  anariRelease(dev, instance);
  anariRelease(dev, light);
  anariRelease(dev, camera);
  anariRelease(dev, dev);
  anariUnloadLibrary(lib);

  free(base);
  free(positions);
  free(colors);

  printf("bigcloud: done after %d frames\n", frameIdx);
  return 0;
}
