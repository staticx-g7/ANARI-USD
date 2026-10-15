// Copyright 2026 JUSYNC contributors


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

// Same launcher variables the device itself reads for rank wiring; run under
// mpirun and each process gets its own piece of the cloud.
static int envRank()
{
  const char *v = getenv("SLURM_PROCID");
  if (!v) v = getenv("OMPI_COMM_WORLD_RANK");
  if (!v) v = getenv("PMI_RANK");
  return v ? atoi(v) : 0;
}

static int envWorldSize()
{
  const char *v = getenv("SLURM_NTASKS");
  if (!v) v = getenv("OMPI_COMM_WORLD_SIZE");
  if (!v) v = getenv("PMI_SIZE");
  return v ? atoi(v) : 1;
}

int main(int argc, const char **argv)
{
  parseArgs(argc, argv);

  const long numPoints = envLong("ANARI_BIG_POINTS", 10000000L);
  const long fps = envLong("ANARI_BIG_FPS", 2L);
  const long seconds = envLong("ANARI_BIG_SECONDS", 0L);
  const long freezeSec = envLong("ANARI_BIG_FREEZE_SEC", 0L);
  const int useColors = envLong("ANARI_BIG_COLORS", 0L) != 0;
  float radius = (float)envLong("ANARI_BIG_RADIUS", 1L);
  const int rank = envRank();
  const int worldSize = envWorldSize() > 0 ? envWorldSize() : 1;
  // Each rank owns a separate slice, so a rank that is dropped, hidden or
  // overwritten on the client is visible as a missing piece instead of a subtle
  // double image.
  const double rankSpread = envLong("ANARI_BIG_RANK_SPREAD", 300L);
  const float rankOffset = (float)((rank - 0.5 * (worldSize - 1)) * rankSpread);
  // 0: keep republishing at one time (replace-in-place streaming). 1: advance
  // usd::time so every step gets its own clip and .geo key.
  const int stepTime = envLong("ANARI_BIG_TIME_STEP", 0L) != 0;
  const double timeDt = (double)envLong("ANARI_BIG_DT", 5L) / 10.0;

  if (numPoints < 1) {
    fprintf(stderr, "ANARI_BIG_POINTS must be >= 1\n");
    return 1;
  }

  printf("bigcloud: rank %d/%d, %ld points, %ld fps, %s, colors=%d, offset %.0f, %s\n",
      rank, worldSize, numPoints, fps, seconds > 0 ? "limited" : "infinite", useColors,
      rankOffset, stepTime ? "advancing time" : "single time");
  fflush(stdout);

  float *base = (float *)malloc((size_t)numPoints * 3 * sizeof(float));
  float *positions = (float *)malloc((size_t)numPoints * 3 * sizeof(float));
  float *colors = NULL;
  if (!base || !positions) {
    fprintf(stderr, "bigcloud: out of memory for %ld points\n", numPoints);
    return 1;
  }
  if (useColors)
    colors = (float *)malloc((size_t)numPoints * 3 * sizeof(float));

  srand(42 + rank);
  for (long i = 0; i < numPoints * 3; ++i)
    base[i] = ((rand() / (float)RAND_MAX) * 2.0f - 1.0f) * 100.0f;
  for (long i = 0; i < numPoints; ++i)
    base[i * 3 + 0] += rankOffset;
  for (long i = 0; i < numPoints * 3; ++i)
    positions[i] = base[i];
  if (colors) {
    for (long i = 0; i < numPoints; ++i) {
      colors[i * 3 + 0] = (float)((i * 2654435761u >> 16) & 0xFF) / 255.0f;
      colors[i * 3 + 1] = (float)((i * 40503u >> 8) & 0xFF) / 255.0f;
      colors[i * 3 + 2] = 0.75f;
    }
  }

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

  // Distinct tint per rank, so a stale or missing rank is obvious rather than a
  // subtle overlap of two identical clouds.
  float kd[3];
  const float hue = rank * 0.37f * 3.14159f * 2.0f;
  kd[0] = 0.35f + 0.65f * fabsf(sinf(hue));
  kd[1] = 0.35f + 0.65f * fabsf(sinf(hue + 2.094f));
  kd[2] = 0.35f + 0.65f * fabsf(sinf(hue + 4.189f));
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

  anariSetParameter(dev, frame, "world", ANARI_WORLD, &world);
  anariSetParameter(dev, frame, "camera", ANARI_CAMERA, &camera);
  anariCommitParameters(dev, frame);

  int frameIdx = 0;
  int frozen = 0;
  for (;;)
  {
    const double commitStart = nowMs();

    if (!frozen && freezeSec > 0 && (nowMs() - startMs) > freezeSec * 1000.0) {
      frozen = 1;
      printf("bigcloud: FROZEN at frame %d - positions static, commits continue\n",
          frameIdx);
      fflush(stdout);
    }

    if (!frozen) {
      const float scale = 1.0f + 0.15f * sinf(frameIdx * 0.35f);
      for (long i = 0; i < numPoints * 3; ++i)
        positions[i] = base[i] * scale;
    }

    ANARIArray1D posArray = anariNewArray1D(dev, positions, 0, 0, ANARI_FLOAT32_VEC3,
        (uint64_t)numPoints);
    anariCommitParameters(dev, posArray);
    anariSetParameter(dev, mesh, "vertex.position", ANARI_ARRAY, &posArray);
    anariRelease(dev, posArray);

    const double frameTime = stepTime ? timeZero + frameIdx * timeDt : timeZero;
    anariSetParameter(dev, dev, "usd::time", ANARI_FLOAT64, &frameTime);
    anariSetParameter(dev, mesh, "usd::time", ANARI_FLOAT64, &frameTime);
    anariCommitParameters(dev, dev);

    anariCommitParameters(dev, mesh);
    anariCommitParameters(dev, surface);
    anariCommitParameters(dev, group);
    anariCommitParameters(dev, instance);
    anariCommitParameters(dev, world);

    anariRenderFrame(dev, frame);
    anariFrameReady(dev, frame, ANARI_WAIT);

    const double elapsed = nowMs() - commitStart;
    printf("bigcloud: rank %d frame %d t=%.2f commit->flush %.1f ms (%.1f MB positions%s)\n",
        rank, frameIdx, frameTime, elapsed,
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
