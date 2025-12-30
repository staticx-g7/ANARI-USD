#pragma once

#include <string>
#include <cstdint>

struct UsdBridgeStreamConfig
{
  // === Master Controls ===
  bool EnableStreaming = true;        // Master switch for all streaming
  bool StreamGeometry = true;         // Stream mesh data
  bool StreamTextures = true;         // Stream texture data
  
  // === Chunking Parameters ===
  size_t PointsPerChunk = 50000;      // Number of points per geometry chunk
  bool StreamIncrementally = true;    // true = send each chunk immediately and free
                                      // false = collect all chunks, send as batch
  
  // === Arrow Flight Connection Settings ===
  std::string Host = "172.31.144.1";     // Flight server host
  uint16_t Port = 8815;               // Flight server port
  bool UseTLS = false;                // Use TLS/SSL (wss:// equivalent)
  
  // === Debug & Logging ===
  bool EnableDebugLogging = true;     // Print debug messages to ParaView console
  bool LogChunkDetails = false;       // Log every chunk (verbose, useful for debugging)
};
