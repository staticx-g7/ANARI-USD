#pragma once

#include "UsdBridgeData.h"
#include "UsdBridgeStreamConfig.h"

#include <memory>
#include <string>
#include <functional>

// Forward declarations
namespace arrow {
  class Schema;
  namespace flight {
    class FlightClient;
    class FlightDescriptor;
  }
}

class UsdBridgeArrowStreamer
{
public:
  explicit UsdBridgeArrowStreamer(const UsdBridgeStreamConfig& cfg);
  ~UsdBridgeArrowStreamer();

  bool Initialize();
  void Shutdown();
  bool IsActive() const;

  // Stream geometry data using Arrow Flight
  bool StreamGeometry(const std::string& primName,
                     const UsdBridgeMeshData& geomData,
                     double timeStep);

  // Stream texture data using Arrow Flight
  bool StreamTexture(const std::string& texName,
                    const UsdBridgeSamplerData& samplerData,
                    const void* convertedImage,
                    int width, int height, int numComponents,
                    double timeStep);

  // Optional logging callback
  using LogCallback = std::function<void(const std::string& message)>;
  void SetLogCallback(LogCallback callback);

private:
  UsdBridgeStreamConfig Config;
  
  std::shared_ptr<arrow::Schema> GeometrySchema;
  std::shared_ptr<arrow::Schema> TextureSchema;
  
  struct Impl;
  std::unique_ptr<Impl> Pimpl;
  
  void InitSchemas();
  
  LogCallback OnLog;
  void Log(const std::string& msg);
  
  // Flight connection management
  bool ConnectToFlightServer();
  void DisconnectFromFlightServer();
};
