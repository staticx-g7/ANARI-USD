// Copyright 2020 The Khronos Group

#ifndef UsdMpiController_h
#define UsdMpiController_h

#include "UsdBridge/Common/UsdBridgeParallelController.h"

#include <memory>

class UsdMpiController : public UsdBridgeParallelController
{
public:
  explicit UsdMpiController(const void* mpiCommPtr);
  ~UsdMpiController() override;

  static std::unique_ptr<UsdMpiController> CreateDefault();

  int GetRank() const override;
  int GetSize() const override;
  void BroadcastInt(int& value) override;
  void Barrier() override;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

#endif
