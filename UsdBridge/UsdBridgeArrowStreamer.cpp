// Copyright 2020 The Khronos Group
// SPDX-License-Identifier: Apache-2.0

#include "UsdBridgeArrowStreamer.h"

#include <arrow/api.h>
#include <arrow/ipc/api.h>
#include <arrow/io/api.h>

#include <cstdio>
#include <algorithm>

struct UsdBridgeArrowStreamer::Impl
{
  std::string OutputDir = "./";
};

UsdBridgeArrowStreamer::UsdBridgeArrowStreamer(const UsdBridgeStreamConfig& cfg)
  : Config(cfg), Pimpl(std::make_unique<Impl>())
{
}

UsdBridgeArrowStreamer::~UsdBridgeArrowStreamer()
{
  Shutdown();
}

bool UsdBridgeArrowStreamer::Initialize()
{
  if (!Config.EnableStreaming)
    return false;

  InitSchemas();
  return true;
}

void UsdBridgeArrowStreamer::Shutdown()
{
}

bool UsdBridgeArrowStreamer::IsActive() const
{
  return Config.EnableStreaming;
}

void UsdBridgeArrowStreamer::InitSchemas()
{
  using arrow::field;
  using arrow::int32;
  using arrow::float64;
  using arrow::utf8;
  using arrow::binary;

  // Texture schema
  TextureSchema = arrow::schema({
    field("name",    utf8()),
    field("time",    float64()),
    field("width",   int32()),
    field("height",  int32()),
    field("channels",int32()),
    field("data",    binary())
  });
}

bool UsdBridgeArrowStreamer::StreamGeometry(const std::string& primName,
                                            const UsdBridgeMeshData& geomData,
                                            double timeStep)
{
  if (!IsActive() || !Config.StreamGeometry)
    return false;

  arrow::MemoryPool* pool = arrow::default_memory_pool();

  // Schema: each row is a chunk
  auto schema = arrow::schema({
    arrow::field("prim_name", arrow::utf8()),
    arrow::field("time", arrow::float64()),
    arrow::field("chunk_id", arrow::int32()),
    arrow::field("total_chunks", arrow::int32()),
    arrow::field("point_offset", arrow::int64()),      // NEW: global offset for this chunk's points
    arrow::field("points", arrow::list(arrow::float32())),
    arrow::field("indices", arrow::list(arrow::int32()))  // Global indices
  });

  auto sinkRes = arrow::io::BufferOutputStream::Create();
  if (!sinkRes.ok())
    return false;
  auto sink = *sinkRes;
  auto wrRes = arrow::ipc::MakeStreamWriter(sink, schema);
  if (!wrRes.ok())
    return false;
  auto writer = *wrRes;

  // Chunking parameters
  const size_t POINTS_PER_CHUNK = 50000;
  const float* pts = static_cast<const float*>(geomData.Points);
  const int32_t* idx = static_cast<const int32_t*>(geomData.Indices);

  size_t totalChunks = (geomData.NumPoints + POINTS_PER_CHUNK - 1) / POINTS_PER_CHUNK;

  // Write chunks
  for (size_t chunkIdx = 0; chunkIdx < totalChunks; ++chunkIdx)
  {
    size_t startPt = chunkIdx * POINTS_PER_CHUNK;
    size_t endPt = std::min(startPt + POINTS_PER_CHUNK, static_cast<size_t>(geomData.NumPoints));
    size_t numPts = endPt - startPt;

    // Metadata
    arrow::StringBuilder nameB(pool);
    nameB.Append(primName);
    std::shared_ptr<arrow::Array> nameArr;
    nameB.Finish(&nameArr);

    arrow::DoubleBuilder timeB(pool);
    timeB.Append(timeStep);
    std::shared_ptr<arrow::Array> timeArr;
    timeB.Finish(&timeArr);

    arrow::Int32Builder chunkIdB(pool), totalChunksB(pool);
    chunkIdB.Append(static_cast<int32_t>(chunkIdx));
    totalChunksB.Append(static_cast<int32_t>(totalChunks));
    std::shared_ptr<arrow::Array> chunkIdArr, totalChunksArr;
    chunkIdB.Finish(&chunkIdArr);
    totalChunksB.Finish(&totalChunksArr);

    arrow::Int64Builder offsetB(pool);
    offsetB.Append(static_cast<int64_t>(startPt));
    std::shared_ptr<arrow::Array> offsetArr;
    offsetB.Finish(&offsetArr);

    // Points (flat xyz for this chunk)
    arrow::ListBuilder ptsListB(pool, std::make_shared<arrow::FloatBuilder>(pool));
    auto* ptsValB = static_cast<arrow::FloatBuilder*>(ptsListB.value_builder());
    ptsListB.Append();
    ptsValB->AppendValues(pts + startPt * 3, numPts * 3);
    std::shared_ptr<arrow::Array> ptsListArr;
    ptsListB.Finish(&ptsListArr);

    // Indices: global indices for triangles touching this chunk
    arrow::ListBuilder idxListB(pool, std::make_shared<arrow::Int32Builder>(pool));
    auto* idxValB = static_cast<arrow::Int32Builder*>(idxListB.value_builder());
    idxListB.Append();
    
    // Filter triangles: include if ANY vertex is in [startPt, endPt)
    for (uint64_t i = 0; i < geomData.NumIndices; i += 3)
    {
      int32_t i0 = idx[i];
      int32_t i1 = idx[i + 1];
      int32_t i2 = idx[i + 2];
      
      // Include triangle if any vertex is in this chunk's point range
      if ((i0 >= static_cast<int32_t>(startPt) && i0 < static_cast<int32_t>(endPt)) ||
          (i1 >= static_cast<int32_t>(startPt) && i1 < static_cast<int32_t>(endPt)) ||
          (i2 >= static_cast<int32_t>(startPt) && i2 < static_cast<int32_t>(endPt)))
      {
        idxValB->Append(i0);  // Global index
        idxValB->Append(i1);
        idxValB->Append(i2);
      }
    }
    std::shared_ptr<arrow::Array> idxListArr;
    idxListB.Finish(&idxListArr);

    auto batch = arrow::RecordBatch::Make(
      schema, 1,
      {nameArr, timeArr, chunkIdArr, totalChunksArr, offsetArr, ptsListArr, idxListArr});

    if (!writer->WriteRecordBatch(*batch).ok())
      return false;
  }

  if (!writer->Close().ok())
    return false;
  auto bufRes = sink->Finish();
  if (!bufRes.ok())
    return false;
  auto buf = *bufRes;

  std::string fileName = Pimpl->OutputDir + primName + "_" +
                         std::to_string(static_cast<int>(timeStep)) + ".geom.arrow";
  FILE* f = std::fopen(fileName.c_str(), "wb");
  if (!f)
    return false;
  std::fwrite(buf->data(), 1, buf->size(), f);
  std::fclose(f);
  return true;
}


bool UsdBridgeArrowStreamer::StreamTexture(const std::string& texName,
                                           const UsdBridgeSamplerData& samplerData,
                                           const void* convertedImage,
                                           int width, int height, int numComponents,
                                           double timeStep)
{
  if (!IsActive() || !Config.StreamTextures)
    return false;

  size_t imageSize = static_cast<size_t>(width) * height * numComponents;
  if (!convertedImage || imageSize == 0)
    return false;

  arrow::MemoryPool* pool = arrow::default_memory_pool();

  arrow::StringBuilder nameB(pool);
  if (!nameB.Append(texName).ok())
    return false;
  std::shared_ptr<arrow::Array> nameArr;
  if (!nameB.Finish(&nameArr).ok())
    return false;

  arrow::DoubleBuilder timeB(pool);
  if (!timeB.Append(timeStep).ok())
    return false;
  std::shared_ptr<arrow::Array> timeArr;
  if (!timeB.Finish(&timeArr).ok())
    return false;

  arrow::Int32Builder wB(pool), hB(pool), cB(pool);
  if (!wB.Append(width).ok())
    return false;
  if (!hB.Append(height).ok())
    return false;
  if (!cB.Append(numComponents).ok())
    return false;
  std::shared_ptr<arrow::Array> wArr, hArr, cArr;
  if (!wB.Finish(&wArr).ok())
    return false;
  if (!hB.Finish(&hArr).ok())
    return false;
  if (!cB.Finish(&cArr).ok())
    return false;

  arrow::BinaryBuilder dataB(pool);
  if (!dataB.Append(reinterpret_cast<const uint8_t*>(convertedImage), imageSize).ok())
    return false;
  std::shared_ptr<arrow::Array> dataArr;
  if (!dataB.Finish(&dataArr).ok())
    return false;

  auto batch = arrow::RecordBatch::Make(
    TextureSchema, 1,
    {nameArr, timeArr, wArr, hArr, cArr, dataArr});

  auto sinkRes = arrow::io::BufferOutputStream::Create();
  if (!sinkRes.ok())
    return false;
  auto sink = *sinkRes;
  auto wrRes = arrow::ipc::MakeStreamWriter(sink, TextureSchema);
  if (!wrRes.ok())
    return false;
  auto writer = *wrRes;
  if (!writer->WriteRecordBatch(*batch).ok())
    return false;
  if (!writer->Close().ok())
    return false;
  auto bufRes = sink->Finish();
  if (!bufRes.ok())
    return false;
  auto buf = *bufRes;

  std::string fileName = Pimpl->OutputDir + texName + "_" +
                         std::to_string(static_cast<int>(timeStep)) + ".tex.arrow";
  FILE* f = std::fopen(fileName.c_str(), "wb");
  if (!f)
    return false;
  std::fwrite(buf->data(), 1, buf->size(), f);
  std::fclose(f);
  return true;
}
