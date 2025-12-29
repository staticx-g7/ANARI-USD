#pragma once
#include <string>

struct UsdBridgeStreamConfig
{
  bool EnableStreaming = true;      // master switch
  bool StreamGeometry = true;
  bool StreamTextures = true;

  std::string Host = "127.0.0.1";    // WebSocket peer
  uint16_t    Port = 9002;
};
