// Copyright 2020 The Khronos Group
// SPDX-License-Identifier: Apache-2.0

#include "UsdBridgeArrowStreamer.h"

#include <arrow/api.h>
#include <arrow/flight/api.h>

#include <algorithm>
#include <sstream>
#include <vector>

struct UsdBridgeArrowStreamer::Impl
{
  std::unique_ptr<arrow::flight::FlightClient> FlightClient;
  bool IsConnected = false;
  
  // For batched mode: accumulate all chunks
  std::vector<std::shared_ptr<arrow::RecordBatch>> BatchedChunks;
  std::string CurrentPrimName;
  int CurrentTotalChunks = 0;
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
  {
    if (Config.EnableDebugLogging)
      Log("[ArrowStreamer] Streaming disabled in config");
    return false;
  }

  InitSchemas();
  
  // Connect to Arrow Flight server
  if (!ConnectToFlightServer())
  {
    Log("[ArrowStreamer] Failed to connect to Flight server");
    return false;
  }
  
  if (Config.EnableDebugLogging)
  {
    std::ostringstream initLog;
    initLog << "[ArrowStreamer] Initialized successfully"
            << " (PointsPerChunk=" << Config.PointsPerChunk
            << ", Incremental=" << (Config.StreamIncrementally ? "true" : "false")
            << ", Server=" << Config.Host << ":" << Config.Port << ")";
    Log(initLog.str());
  }
  
  return true;
}

void UsdBridgeArrowStreamer::Shutdown()
{
  DisconnectFromFlightServer();
  
  if (Pimpl)
    Pimpl->BatchedChunks.clear();
  
  if (Config.EnableDebugLogging)
    Log("[ArrowStreamer] Shutdown");
}

bool UsdBridgeArrowStreamer::IsActive() const
{
  return Config.EnableStreaming && Pimpl->IsConnected;
}

void UsdBridgeArrowStreamer::InitSchemas()
{
  using arrow::field;
  using arrow::int32;
  using arrow::int64;
  using arrow::float32;
  using arrow::float64;
  using arrow::utf8;
  using arrow::binary;

  // Geometry schema (same as before, but will use Flight)
  GeometrySchema = arrow::schema({
    field("prim_name", utf8()),
    field("time", float64()),
    field("chunk_id", int32()),
    field("total_chunks", int32()),
    field("point_offset", int64()),
    field("points", arrow::list(float32())),
    field("indices", arrow::list(int32()))
  });

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

void UsdBridgeArrowStreamer::Log(const std::string& msg)
{
  if (OnLog)
    OnLog(msg);
}

bool UsdBridgeArrowStreamer::ConnectToFlightServer()
{
  arrow::flight::Location location;
  arrow::Status status;
  
  if (Config.UseTLS)
  {
    status = arrow::flight::Location::ForGrpcTls(Config.Host, Config.Port).Value(&location);
  }
  else
  {
    status = arrow::flight::Location::ForGrpcTcp(Config.Host, Config.Port).Value(&location);
  }
  
  if (!status.ok())
  {
    std::ostringstream err;
    err << "[ArrowStreamer] Failed to create location: " << status.ToString();
    Log(err.str());
    return false;
  }
  
  auto clientResult = arrow::flight::FlightClient::Connect(location);
  if (!clientResult.ok())
  {
    std::ostringstream err;
    err << "[ArrowStreamer] Failed to connect to Flight server at " 
        << Config.Host << ":" << Config.Port << " - " << clientResult.status().ToString();
    Log(err.str());
    return false;
  }
  
  Pimpl->FlightClient = std::move(*clientResult);
  Pimpl->IsConnected = true;
  
  return true;
}

void UsdBridgeArrowStreamer::DisconnectFromFlightServer()
{
  if (Pimpl->FlightClient)
  {
    Pimpl->FlightClient.reset();
    Pimpl->IsConnected = false;
  }
}

bool UsdBridgeArrowStreamer::StreamGeometry(const std::string& primName,
                                            const UsdBridgeMeshData& geomData,
                                            double timeStep)
{
  if (!IsActive() || !Config.StreamGeometry)
    return false;

  arrow::MemoryPool* pool = arrow::default_memory_pool();
  
  const size_t POINTS_PER_CHUNK = Config.PointsPerChunk;
  const float* pts = static_cast<const float*>(geomData.Points);
  const int32_t* idx = static_cast<const int32_t*>(geomData.Indices);

  size_t totalChunks = (geomData.NumPoints + POINTS_PER_CHUNK - 1) / POINTS_PER_CHUNK;

  if (Config.EnableDebugLogging)
  {
    std::ostringstream logMsg;
    logMsg << "[ArrowStreamer] Streaming geometry '" << primName 
           << "': " << geomData.NumPoints << " points, " 
           << geomData.NumIndices << " indices, " 
           << totalChunks << " chunks"
           << (Config.StreamIncrementally ? " (incremental)" : " (batched)");
    Log(logMsg.str());
  }

  // Clear batch storage if starting new geometry in batched mode
  if (!Config.StreamIncrementally)
  {
    Pimpl->BatchedChunks.clear();
    Pimpl->CurrentPrimName = primName;
    Pimpl->CurrentTotalChunks = static_cast<int>(totalChunks);
  }

  // Create Flight descriptor for this geometry
  arrow::flight::FlightDescriptor descriptor;
  descriptor.type = arrow::flight::FlightDescriptor::PATH;
  descriptor.path = {"geometry", primName, std::to_string(timeStep)};

  // Process each chunk
  std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
  
  for (size_t chunkIdx = 0; chunkIdx < totalChunks; ++chunkIdx)
  {
    size_t startPt = chunkIdx * POINTS_PER_CHUNK;
    size_t endPt = std::min(startPt + POINTS_PER_CHUNK, static_cast<size_t>(geomData.NumPoints));
    size_t numPts = endPt - startPt;

    if (Config.LogChunkDetails)
    {
      std::ostringstream chunkLog;
      chunkLog << "[ArrowStreamer] Processing chunk " << chunkIdx 
               << "/" << (totalChunks-1) << ": " << numPts << " points";
      Log(chunkLog.str());
    }

    // Build record batch for this chunk
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

    // Points for THIS chunk only
    arrow::ListBuilder ptsListB(pool, std::make_shared<arrow::FloatBuilder>(pool));
    auto* ptsValB = static_cast<arrow::FloatBuilder*>(ptsListB.value_builder());
    ptsListB.Append();
    ptsValB->AppendValues(pts + startPt * 3, numPts * 3);
    std::shared_ptr<arrow::Array> ptsListArr;
    ptsListB.Finish(&ptsListArr);

    // Indices
    arrow::ListBuilder idxListB(pool, std::make_shared<arrow::Int32Builder>(pool));
    auto* idxValB = static_cast<arrow::Int32Builder*>(idxListB.value_builder());
    idxListB.Append();
    
    for (uint64_t i = 0; i < geomData.NumIndices; i += 3)
    {
      int32_t i0 = idx[i];
      int32_t i1 = idx[i + 1];
      int32_t i2 = idx[i + 2];
      
      if ((i0 >= static_cast<int32_t>(startPt) && i0 < static_cast<int32_t>(endPt)) ||
          (i1 >= static_cast<int32_t>(startPt) && i1 < static_cast<int32_t>(endPt)) ||
          (i2 >= static_cast<int32_t>(startPt) && i2 < static_cast<int32_t>(endPt)))
      {
        idxValB->Append(i0);
        idxValB->Append(i1);
        idxValB->Append(i2);
      }
    }
    std::shared_ptr<arrow::Array> idxListArr;
    idxListB.Finish(&idxListArr);

    auto batch = arrow::RecordBatch::Make(
      GeometrySchema, 1,
      {nameArr, timeArr, chunkIdArr, totalChunksArr, offsetArr, ptsListArr, idxListArr});

    // DECISION POINT: Incremental or Batched?
    if (Config.StreamIncrementally)
    {
      // Send immediately via Flight (Arrow 22.x API)
      batches.clear();
      batches.push_back(batch);
      
      // NEW API: DoPut returns a DoPutResult with writer
      auto resultOrError = Pimpl->FlightClient->DoPut(descriptor, GeometrySchema);
      if (!resultOrError.ok())
      {
        Log("[ArrowStreamer] DoPut failed: " + resultOrError.status().ToString());
        return false;
      }
      
      auto doPutResult = std::move(*resultOrError);
      auto status = doPutResult.writer->WriteRecordBatch(*batch);
      if (!status.ok())
      {
        Log("[ArrowStreamer] WriteRecordBatch failed: " + status.ToString());
        return false;
      }
      
      status = doPutResult.writer->DoneWriting();
      if (!status.ok())
      {
        Log("[ArrowStreamer] DoneWriting failed: " + status.ToString());
        return false;
      }
      
      status = doPutResult.writer->Close();
      if (!status.ok())
      {
        Log("[ArrowStreamer] Close failed: " + status.ToString());
        return false;
      }
    }
    else
    {
      // BATCHED MODE: Keep in memory
      Pimpl->BatchedChunks.push_back(batch);
    }
  }

  // If batched mode, send all chunks now
  if (!Config.StreamIncrementally)
  {
    if (Config.EnableDebugLogging)
    {
      std::ostringstream batchLog;
      batchLog << "[ArrowStreamer] Sending " << Pimpl->BatchedChunks.size() 
               << " batched chunks for '" << primName << "'";
      Log(batchLog.str());
    }

    auto resultOrError = Pimpl->FlightClient->DoPut(descriptor, GeometrySchema);
    if (!resultOrError.ok())
    {
      Log("[ArrowStreamer] DoPut failed: " + resultOrError.status().ToString());
      return false;
    }
    
    auto doPutResult = std::move(*resultOrError);
    for (const auto& batch : Pimpl->BatchedChunks)
    {
      auto status = doPutResult.writer->WriteRecordBatch(*batch);
      if (!status.ok())
      {
        Log("[ArrowStreamer] WriteRecordBatch failed: " + status.ToString());
        return false;
      }
    }
    
    auto status = doPutResult.writer->DoneWriting();
    if (!status.ok())
    {
      Log("[ArrowStreamer] DoneWriting failed: " + status.ToString());
      return false;
    }
    
    status = doPutResult.writer->Close();
    if (!status.ok())
    {
      Log("[ArrowStreamer] Close failed: " + status.ToString());
      return false;
    }
    
    // Now free all buffers
    Pimpl->BatchedChunks.clear();
  }

  if (Config.EnableDebugLogging)
  {
    Log("[ArrowStreamer] Geometry streaming complete");
  }

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

  if (Config.EnableDebugLogging)
  {
    std::ostringstream logMsg;
    logMsg << "[ArrowStreamer] Streaming texture '" << texName 
           << "': " << width << "x" << height 
           << " (" << numComponents << " channels, " 
           << imageSize << " bytes)";
    Log(logMsg.str());
  }

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

  // Send via Flight (Arrow 22.x API)
  arrow::flight::FlightDescriptor descriptor;
  descriptor.type = arrow::flight::FlightDescriptor::PATH;
  descriptor.path = {"texture", texName, std::to_string(timeStep)};
  
  auto resultOrError = Pimpl->FlightClient->DoPut(descriptor, TextureSchema);
  if (!resultOrError.ok())
  {
    Log("[ArrowStreamer] Texture DoPut failed: " + resultOrError.status().ToString());
    return false;
  }
  
  auto doPutResult = std::move(*resultOrError);
  auto status = doPutResult.writer->WriteRecordBatch(*batch);
  if (!status.ok())
  {
    Log("[ArrowStreamer] Texture WriteRecordBatch failed: " + status.ToString());
    return false;
  }
  
  status = doPutResult.writer->DoneWriting();
  if (!status.ok())
  {
    Log("[ArrowStreamer] Texture DoneWriting failed: " + status.ToString());
    return false;
  }
  
  status = doPutResult.writer->Close();
  if (!status.ok())
  {
    Log("[ArrowStreamer] Texture Close failed: " + status.ToString());
    return false;
  }

  if (Config.EnableDebugLogging)
  {
    Log("[ArrowStreamer] Texture streaming complete");
  }

  return true;
}

void UsdBridgeArrowStreamer::SetLogCallback(LogCallback callback)
{
  OnLog = callback;
}
