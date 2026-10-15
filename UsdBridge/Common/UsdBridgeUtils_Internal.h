// Copyright 2020 The Khronos Group

#ifndef UsdBridgeUtils_Internal_h
#define UsdBridgeUtils_Internal_h

#include "UsdBridgeData.h"

#include <cstring>

namespace
{
  inline bool strEquals(const char* arg0, const char* arg1)
  {
    return strcmp(arg0, arg1) == 0;
  }
}

#endif