// Copyright 2020 The Khronos Group
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "UsdBridgeData.h"
#include "UsdBridgeStreamConfig.h"

#include <memory>
#include <string>

namespace arrow {
  class Schema;
}

class UsdBridgeArrowStreamer
{
public:
  explicit UsdBridgeArrowStreamer(const UsdBridgeStreamConfig& cfg);
  ~UsdBridgeArrowStreamer();

  bool Initialize();
  void Shutdown();

  bool IsActive() const;

  bool StreamGeometry(const std::string& primName,
                      const UsdBridgeMeshData& geomData,
                      double timeStep);

  bool StreamTexture(const std::string& texName,
                     const UsdBridgeSamplerData& samplerData,
                     const void* convertedImage,
                     int width, int height, int numComponents,
                     double timeStep);

private:
  UsdBridgeStreamConfig Config;

  std::shared_ptr<arrow::Schema> GeometrySchema;
  std::shared_ptr<arrow::Schema> TextureSchema;

  struct Impl;
  std::unique_ptr<Impl> Pimpl;

  void InitSchemas();
};
