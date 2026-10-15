// Copyright 2026 JUSYNC contributors
// SPDX-License-Identifier: Apache-2.0

// Sender example: one time-varying triangle MESH and one time-varying POINT
// CLOUD in the same scene, so both client paths are exercised at once (USD mesh
// clips for the surface, RawGeo .geo for the points when JUSYNC_RAWGEO=1).
//
// Every frame commits ONE time step under its own usd::time, so the store fills
// with clips/<name>_Geom__<t> per element and the Unreal timeline gets real
// steps to play. The time key cycles over ANARI_DEMO_STEPS values while the
// animation phase keeps advancing, so each step is rewritten with fresh
// geometry (a running sim captured into a bounded timeline) rather than going
// idle once the last step has been written.
//
//   ANARI_DEMO_POINTS     point count                 (default 200000)
//   ANARI_DEMO_MESH_DIM   grid vertices per side       (default 96)
//   ANARI_DEMO_STEPS      time steps in the timeline    (default 8)
//   ANARI_DEMO_DT         seconds between steps         (default 0.5)
//   ANARI_DEMO_FPS        commits per second            (default 2)
//   ANARI_DEMO_SECONDS    run duration, 0 = forever     (default 0)
//   ANARI_DEMO_COLORS     0 disables vertex colors      (default 1)
//   ANARI_DEMO_RADIUS     point radius                  (default 1.0)

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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
  return (double)GetTickCount64();
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

static double envDouble(const char *name, double dflt)
{
  const char *v = getenv(name);
  return v ? atof(v) : dflt;
}

// One animated surface: grid in the XY plane, z displaced by a travelling
// ripple. Returns positions (dim*dim*3), per-vertex normals, and NaN-free
// normals even where the ripple gradient is singular.
static void buildMeshGrid(int dim, float extent, float phase,
    float *pos, float *nrm, float *col)
{
  const float inv = (dim > 1) ? (2.0f / (float)(dim - 1)) : 0.0f;
  for (int j = 0; j < dim; ++j)
  {
    for (int i = 0; i < dim; ++i)
    {
      const int idx = j * dim + i;
      const float x = (-1.0f + (float)i * inv) * extent;
      const float y = (-1.0f + (float)j * inv) * extent;
      const float r = sqrtf(x * x + y * y) + 1e-4f;
      const float k = 0.05f;
      const float fall = expf(-r * r * (0.6f / (extent * extent)));
      const float a = 0.22f * extent * fall;
      const float arg = r * k - phase * 1.6f;
      const float z = a * sinf(arg);

      // dz/dr with the envelope included, then cartesian gradient.
      const float dzdr = a * (k * cosf(arg) - 2.0f * r * (0.6f / (extent * extent)) * sinf(arg));
      const float dzdx = dzdr * (x / r);
      const float dzdy = dzdr * (y / r);
      const float nl = sqrtf(dzdx * dzdx + dzdy * dzdy + 1.0f);

      pos[idx * 3 + 0] = x;
      pos[idx * 3 + 1] = y;
      pos[idx * 3 + 2] = z;
      nrm[idx * 3 + 0] = -dzdx / nl;
      nrm[idx * 3 + 1] = -dzdy / nl;
      nrm[idx * 3 + 2] = 1.0f / nl;

      const float h = 0.5f + 0.5f * (z / (0.25f * extent + 1e-6f));
      col[idx * 4 + 0] = 0.25f + 0.65f * h;
      col[idx * 4 + 1] = 0.35f + 0.25f * (1.0f - h);
      col[idx * 4 + 2] = 0.95f - 0.55f * h;
      col[idx * 4 + 3] = 1.0f;
    }
  }
}

// Fibonacci-sphere directions, rotated about Y and pulsed: unambiguous motion
// that stays inside a bounded box (the client's spawn grid tolerates overlap,
// but a tight cloud makes "is it actually animating" obvious).
static void buildPointCloud(long n, float radius, float phase,
    const float *dir, float *pos, float *col)
{
  const float ang = phase * 0.6f;
  const float ca = cosf(ang), sa = sinf(ang);
  const float pulse = 1.0f + 0.12f * sinf(phase * 1.1f);
  for (long i = 0; i < n; ++i)
  {
    const float x = dir[i * 3 + 0] * radius * pulse;
    const float y = dir[i * 3 + 1] * radius * pulse;
    const float z = dir[i * 3 + 2] * radius * pulse;
    pos[i * 3 + 0] = x * ca + z * sa;
    pos[i * 3 + 1] = y;
    pos[i * 3 + 2] = -x * sa + z * ca;

    const float t = 0.5f + 0.5f * dir[i * 3 + 1];
    col[i * 3 + 0] = 0.15f + 0.8f * t;
    col[i * 3 + 1] = 0.30f + 0.55f * (1.0f - t);
    col[i * 3 + 2] = 0.95f - 0.70f * t;
  }
}

int main(int argc, const char **argv)
{
  parseArgs(argc, argv);

  const long numPoints = envLong("ANARI_DEMO_POINTS", 200000L);
  const long dim = envLong("ANARI_DEMO_MESH_DIM", 96L);
  const long steps = envLong("ANARI_DEMO_STEPS", 8L);
  const double dt = envDouble("ANARI_DEMO_DT", 0.5);
  const long fps = envLong("ANARI_DEMO_FPS", 2L);
  const long seconds = envLong("ANARI_DEMO_SECONDS", 0L);
  const int useColors = envLong("ANARI_DEMO_COLORS", 1L) != 0;
  const float radius = (float)envDouble("ANARI_DEMO_RADIUS", 1.0);

  if (numPoints < 1 || dim < 2 || steps < 1)
  {
    fprintf(stderr, "demo: ANARI_DEMO_POINTS >= 1, ANARI_DEMO_MESH_DIM >= 2, ANARI_DEMO_STEPS >= 1\n");
    return 1;
  }

  const long nVerts = dim * dim;
  const long nTris = (dim - 1) * (dim - 1) * 2;
  const float extent = 60.0f;

  printf("demo: mesh %ld verts / %ld tris, %ld points, %ld steps @ %.3fs, %ld fps, colors=%d\n",
      nVerts, nTris, numPoints, steps, dt, fps, useColors);
  fflush(stdout);

  float *meshPos = (float *)malloc((size_t)nVerts * 3 * sizeof(float));
  float *meshNrm = (float *)malloc((size_t)nVerts * 3 * sizeof(float));
  float *meshCol = (float *)malloc((size_t)nVerts * 4 * sizeof(float));
  int32_t *tris = (int32_t *)malloc((size_t)nTris * 3 * sizeof(int32_t));
  float *dirs = (float *)malloc((size_t)numPoints * 3 * sizeof(float));
  float *ptPos = (float *)malloc((size_t)numPoints * 3 * sizeof(float));
  float *ptCol = (float *)malloc((size_t)numPoints * 3 * sizeof(float));
  if (!meshPos || !meshNrm || !meshCol || !tris || !dirs || !ptPos || !ptCol)
  {
    fprintf(stderr, "demo: out of memory\n");
    return 1;
  }

  for (long j = 0; j < dim - 1; ++j)
  {
    for (long i = 0; i < dim - 1; ++i)
    {
      const int32_t v00 = (int32_t)(j * dim + i);
      const int32_t v10 = (int32_t)(j * dim + i + 1);
      const int32_t v01 = (int32_t)((j + 1) * dim + i);
      const int32_t v11 = (int32_t)((j + 1) * dim + i + 1);
      const long t = (j * (dim - 1) + i) * 6;
      tris[t + 0] = v00; tris[t + 1] = v10; tris[t + 2] = v11;
      tris[t + 3] = v00; tris[t + 4] = v11; tris[t + 5] = v01;
    }
  }

  // Deterministic Fibonacci sphere (no RNG state to keep reproducible).
  {
    const float ga = 3.14159265358979f * (3.0f - sqrtf(5.0f));
    for (long i = 0; i < numPoints; ++i)
    {
      const float y = 1.0f - 2.0f * ((float)i + 0.5f) / (float)numPoints;
      const float rr = sqrtf(fmaxf(0.0f, 1.0f - y * y));
      const float th = ga * (float)i;
      dirs[i * 3 + 0] = cosf(th) * rr;
      dirs[i * 3 + 1] = y;
      dirs[i * 3 + 2] = sinf(th) * rr;
    }
  }

  ANARILibrary lib = anariLoadLibrary("usd", statusFunc, NULL);
  ANARIDevice dev = anariNewDevice(lib, "usd");
  if (!dev)
  {
    fprintf(stderr, "demo: could not create usd device\n");
    return 1;
  }

  int outputBinary = 1;
  int connLogVerbosity = 0;
  anariSetParameter(dev, dev, "usd::connection.logVerbosity", ANARI_INT32, &connLogVerbosity);
  anariSetParameter(dev, dev, "usd::serialize.outputBinary", ANARI_BOOL, &outputBinary);
  anariCommitParameters(dev, dev);

  float cam_pos[] = {0.f, -40.f, -260.f};
  float cam_view[] = {0.f, 0.15f, 1.f};
  float cam_up[] = {0.f, 1.f, 0.f};
  ANARICamera camera = anariNewCamera(dev, "perspective");
  float aspect = 1.0f;
  anariSetParameter(dev, camera, "aspect", ANARI_FLOAT32, &aspect);
  anariSetParameter(dev, camera, "position", ANARI_FLOAT32_VEC3, cam_pos);
  anariSetParameter(dev, camera, "direction", ANARI_FLOAT32_VEC3, cam_view);
  anariSetParameter(dev, camera, "up", ANARI_FLOAT32_VEC3, cam_up);
  anariCommitParameters(dev, camera);

  ANARIFrame frame = anariNewFrame(dev);
  anariSetParameter(dev, frame, "size", ANARI_UINT32_VEC2, (const uint32_t[]){16, 16});
  ANARIDataType colFormat = ANARI_UFIXED8_RGBA_SRGB;
  anariSetParameter(dev, frame, "channel.color", ANARI_DATA_TYPE, &colFormat);

  ANARIWorld world = anariNewWorld(dev);
  anariSetParameter(dev, world, "name", ANARI_STRING, "demoWorld");

  // --- mesh -----------------------------------------------------------------
  ANARIGeometry mesh = anariNewGeometry(dev, "triangle");
  anariSetParameter(dev, mesh, "name", ANARI_STRING, "demoMesh");

  ANARIArray1D array = anariNewArray1D(dev, meshPos, 0, 0, ANARI_FLOAT32_VEC3, (uint64_t)nVerts);
  anariCommitParameters(dev, array);
  anariSetParameter(dev, mesh, "vertex.position", ANARI_ARRAY, &array);
  anariRelease(dev, array);

  array = anariNewArray1D(dev, meshNrm, 0, 0, ANARI_FLOAT32_VEC3, (uint64_t)nVerts);
  anariCommitParameters(dev, array);
  anariSetParameter(dev, mesh, "vertex.normal", ANARI_ARRAY, &array);
  anariRelease(dev, array);

  if (useColors)
  {
    array = anariNewArray1D(dev, meshCol, 0, 0, ANARI_FLOAT32_VEC4, (uint64_t)nVerts);
    anariCommitParameters(dev, array);
    anariSetParameter(dev, mesh, "vertex.color", ANARI_ARRAY, &array);
    anariRelease(dev, array);
  }

  array = anariNewArray1D(dev, tris, 0, 0, ANARI_INT32_VEC3, (uint64_t)nTris);
  anariCommitParameters(dev, array);
  anariSetParameter(dev, mesh, "primitive.index", ANARI_ARRAY, &array);
  anariRelease(dev, array);

  ANARIMaterial meshMat = anariNewMaterial(dev, "matte");
  anariSetParameter(dev, meshMat, "name", ANARI_STRING, "demoMeshMaterial");
  float meshKd[] = {0.9f, 0.6f, 0.35f};
  anariSetParameter(dev, meshMat, "color", ANARI_FLOAT32_VEC3, meshKd);
  anariCommitParameters(dev, meshMat);

  ANARISurface meshSurface = anariNewSurface(dev);
  anariSetParameter(dev, meshSurface, "name", ANARI_STRING, "demoMeshSurface");
  anariSetParameter(dev, meshSurface, "geometry", ANARI_GEOMETRY, &mesh);
  anariSetParameter(dev, meshSurface, "material", ANARI_MATERIAL, &meshMat);
  anariCommitParameters(dev, meshSurface);

  ANARIGroup meshGroup = anariNewGroup(dev);
  anariSetParameter(dev, meshGroup, "name", ANARI_STRING, "demoMeshGroup");
  array = anariNewArray1D(dev, &meshSurface, 0, 0, ANARI_SURFACE, 1);
  anariCommitParameters(dev, array);
  anariSetParameter(dev, meshGroup, "surface", ANARI_ARRAY, &array);
  anariRelease(dev, array);
  anariCommitParameters(dev, meshGroup);

  ANARIInstance meshInstance = anariNewInstance(dev, "transform");
  anariSetParameter(dev, meshInstance, "name", ANARI_STRING, "demoMeshInstance");
  float meshXform[16] = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, -70.0f, 0.0f, 1.0f };
  anariSetParameter(dev, meshInstance, "transform", ANARI_FLOAT32_MAT4, meshXform);
  anariSetParameter(dev, meshInstance, "group", ANARI_GROUP, &meshGroup);
  anariCommitParameters(dev, meshInstance);

  // --- point cloud ----------------------------------------------------------
  // useUsdGeomPoints=0 keeps this a UsdGeomPointInstancer, which is the shape the
  // RawGeo channel mirrors to clips/<name>_Geom__<t>.geo.
  ANARIGeometry points = anariNewGeometry(dev, "sphere");
  anariSetParameter(dev, points, "name", ANARI_STRING, "demoPoints");
  int useUsdGeomPoints = 0;
  anariSetParameter(dev, points, "usd::useUsdGeomPoints", ANARI_BOOL, &useUsdGeomPoints);
  anariSetParameter(dev, points, "radius", ANARI_FLOAT32, &radius);

  array = anariNewArray1D(dev, ptPos, 0, 0, ANARI_FLOAT32_VEC3, (uint64_t)numPoints);
  anariCommitParameters(dev, array);
  anariSetParameter(dev, points, "vertex.position", ANARI_ARRAY, &array);
  anariRelease(dev, array);

  if (useColors)
  {
    array = anariNewArray1D(dev, ptCol, 0, 0, ANARI_FLOAT32_VEC3, (uint64_t)numPoints);
    anariCommitParameters(dev, array);
    anariSetParameter(dev, points, "vertex.color", ANARI_ARRAY, &array);
    anariRelease(dev, array);
  }

  ANARIMaterial pointMat = anariNewMaterial(dev, "matte");
  anariSetParameter(dev, pointMat, "name", ANARI_STRING, "demoPointsMaterial");
  float ptKd[] = {0.3f, 0.7f, 0.95f};
  anariSetParameter(dev, pointMat, "color", ANARI_FLOAT32_VEC3, ptKd);
  anariCommitParameters(dev, pointMat);

  ANARISurface pointSurface = anariNewSurface(dev);
  anariSetParameter(dev, pointSurface, "name", ANARI_STRING, "demoPointsSurface");
  anariSetParameter(dev, pointSurface, "geometry", ANARI_GEOMETRY, &points);
  anariSetParameter(dev, pointSurface, "material", ANARI_MATERIAL, &pointMat);
  anariCommitParameters(dev, pointSurface);

  ANARIGroup pointGroup = anariNewGroup(dev);
  anariSetParameter(dev, pointGroup, "name", ANARI_STRING, "demoPointsGroup");
  array = anariNewArray1D(dev, &pointSurface, 0, 0, ANARI_SURFACE, 1);
  anariCommitParameters(dev, array);
  anariSetParameter(dev, pointGroup, "surface", ANARI_ARRAY, &array);
  anariRelease(dev, array);
  anariCommitParameters(dev, pointGroup);

  ANARIInstance pointInstance = anariNewInstance(dev, "transform");
  anariSetParameter(dev, pointInstance, "name", ANARI_STRING, "demoPointsInstance");
  float pointXform[16] = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 90.0f, 0.0f, 1.0f };
  anariSetParameter(dev, pointInstance, "transform", ANARI_FLOAT32_MAT4, pointXform);
  anariSetParameter(dev, pointInstance, "group", ANARI_GROUP, &pointGroup);
  anariCommitParameters(dev, pointInstance);

  ANARIInstance instances[2] = {meshInstance, pointInstance};
  array = anariNewArray1D(dev, instances, 0, 0, ANARI_INSTANCE, 2);
  anariCommitParameters(dev, array);
  anariSetParameter(dev, world, "instance", ANARI_ARRAY, &array);
  anariRelease(dev, array);

  ANARILight light = anariNewLight(dev, "hdri");
  anariSetParameter(dev, light, "name", ANARI_STRING, "demoLight");
  float lightRadiance = 1.0f;
  anariSetParameter(dev, light, "radiance", ANARI_FLOAT32, &lightRadiance);
  anariCommitParameters(dev, light);

  array = anariNewArray1D(dev, &light, 0, 0, ANARI_LIGHT, 1);
  anariCommitParameters(dev, array);
  anariSetParameter(dev, world, "light", ANARI_ARRAY, &array);
  anariRelease(dev, array);
  anariCommitParameters(dev, world);

  anariSetParameter(dev, frame, "world", ANARI_WORLD, &world);
  anariSetParameter(dev, frame, "camera", ANARI_CAMERA, &camera);
  anariCommitParameters(dev, frame);

  const long frameBudgetMs = fps > 0 ? 1000 / fps : 500;
  const double startMs = nowMs();

  long frameIdx = 0;
  for (;;)
  {
    const double commitStart = nowMs();

    const double timeValue = (double)(frameIdx % steps) * dt;
    const float phase = (float)frameIdx * 0.35f;

    buildMeshGrid((int)dim, extent, phase, meshPos, meshNrm, meshCol);
    buildPointCloud(numPoints, 45.0f, phase, dirs, ptPos, ptCol);

    ANARIArray1D mPos = anariNewArray1D(dev, meshPos, 0, 0, ANARI_FLOAT32_VEC3, (uint64_t)nVerts);
    anariCommitParameters(dev, mPos);
    anariSetParameter(dev, mesh, "vertex.position", ANARI_ARRAY, &mPos);
    anariRelease(dev, mPos);

    ANARIArray1D mNrm = anariNewArray1D(dev, meshNrm, 0, 0, ANARI_FLOAT32_VEC3, (uint64_t)nVerts);
    anariCommitParameters(dev, mNrm);
    anariSetParameter(dev, mesh, "vertex.normal", ANARI_ARRAY, &mNrm);
    anariRelease(dev, mNrm);

    ANARIArray1D pPos = anariNewArray1D(dev, ptPos, 0, 0, ANARI_FLOAT32_VEC3, (uint64_t)numPoints);
    anariCommitParameters(dev, pPos);
    anariSetParameter(dev, points, "vertex.position", ANARI_ARRAY, &pPos);
    anariRelease(dev, pPos);

    // Device-level time selects the clip/geo key; the geometry-level time is
    // what the bridge stamps on the per-timestep stage.
    anariSetParameter(dev, dev, "usd::time", ANARI_FLOAT64, (void *)&timeValue);
    anariCommitParameters(dev, dev);
    anariSetParameter(dev, mesh, "usd::time", ANARI_FLOAT64, (void *)&timeValue);
    anariSetParameter(dev, points, "usd::time", ANARI_FLOAT64, (void *)&timeValue);

    anariCommitParameters(dev, mesh);
    anariCommitParameters(dev, points);
    anariCommitParameters(dev, meshSurface);
    anariCommitParameters(dev, pointSurface);
    anariCommitParameters(dev, meshGroup);
    anariCommitParameters(dev, pointGroup);
    anariCommitParameters(dev, meshInstance);
    anariCommitParameters(dev, pointInstance);
    anariCommitParameters(dev, world);

    anariRenderFrame(dev, frame);
    anariFrameReady(dev, frame, ANARI_WAIT);

    const double elapsed = nowMs() - commitStart;
    printf("demo: frame %ld step %ld t=%.3f commit->flush %.1f ms (mesh %ld verts, points %ld)\n",
        frameIdx, frameIdx % steps, timeValue, elapsed, nVerts, numPoints);
    fflush(stdout);

    ++frameIdx;

    if (seconds > 0 && (nowMs() - startMs) > (double)seconds * 1000.0)
      break;

    const long wait = frameBudgetMs - (long)(nowMs() - commitStart);
    if (wait > 0)
      sleepMs(wait);
  }

  anariRelease(dev, frame);
  anariRelease(dev, world);
  anariRelease(dev, mesh);
  anariRelease(dev, meshMat);
  anariRelease(dev, meshSurface);
  anariRelease(dev, meshGroup);
  anariRelease(dev, meshInstance);
  anariRelease(dev, points);
  anariRelease(dev, pointMat);
  anariRelease(dev, pointSurface);
  anariRelease(dev, pointGroup);
  anariRelease(dev, pointInstance);
  anariRelease(dev, light);
  anariRelease(dev, camera);
  anariRelease(dev, dev);
  anariUnloadLibrary(lib);

  free(meshPos);
  free(meshNrm);
  free(meshCol);
  free(tris);
  free(dirs);
  free(ptPos);
  free(ptCol);

  printf("demo: done after %ld frames\n", frameIdx);
  return 0;
}
