// Copyright 2020 The Khronos Group

#ifndef UsdBridgeRt_h
#define UsdBridgeRt_h

#include "UsdBridgeData.h"

class UsdBridgeRtInternals;

class UsdBridgeRt
{
  public:
    UsdBridgeRt(const ::PXR_NS::UsdStagePtr& sceneStage, const ::PXR_NS::SdfPath& primPath);
    ~UsdBridgeRt();

    void ReCalc();
    bool ValidPrim();

    template<typename ReturnEltType = UsdBridgeNoneType>
    UsdBridgeSpanI<ReturnEltType>* UpdateUsdAttribute(const UsdBridgeLogObject& logObj, const void* arrayData, UsdBridgeType arrayDataType, size_t arrayNumElements,
      const ::PXR_NS::UsdAttribute& attrib, const ::PXR_NS::UsdTimeCode& timeCode, bool writeToAttrib);

  protected:

    UsdBridgeRtInternals* Internals = nullptr;
};

#endif