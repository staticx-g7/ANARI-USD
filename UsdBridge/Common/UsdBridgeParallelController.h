// Copyright 2020 The Khronos Group

#ifndef UsdBridgeParallelController_h
#define UsdBridgeParallelController_h

class UsdBridgeParallelController
{
public:
  virtual ~UsdBridgeParallelController() = default;

  virtual int GetRank() const = 0;

  virtual int GetSize() const = 0;

  virtual void BroadcastInt(int& value) = 0;

  virtual void Barrier() = 0;
};

#endif
