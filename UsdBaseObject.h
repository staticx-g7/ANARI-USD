// Copyright 2020 The Khronos Group

#pragma once

#include "helium/utility/IntrusivePtr.h"
#include "UsdCommonMacros.h"
#include "UsdParameterizedObject.h"

#include <string>

class UsdDevice;

std::string SanitizeUsdName(const char* name);

std::string UsdNameRegistryClaim(const std::string& desired, const void* owner);

void UsdNameRegistryRelease(const std::string& claimed, const void* owner);

class UsdBaseObject : public helium::RefCounted
{
  public:
    UsdBaseObject(ANARIDataType t, UsdDevice* device = nullptr);

    virtual void filterSetParam(
      const char *name,
      ANARIDataType type,
      const void *mem,
      UsdDevice* device) = 0;

    virtual void filterResetParam(
      const char *name) = 0;

    virtual void resetAllParams() = 0;

    virtual void* getParameter(const char* name, ANARIDataType& returnType) = 0;

    virtual int getProperty(const char *name,
      ANARIDataType type,
      void *mem,
      uint64_t size,
      UsdDevice* device) = 0;

    virtual void commit(UsdDevice* device) = 0;

    virtual void remove(UsdDevice* device) = 0; // Remove any committed data and refs

    ANARIDataType getType() const { return type; }

    void addObserver(UsdBaseObject* observer);
    void removeObserver(UsdBaseObject* observer);
    void notify(UsdBaseObject* caller, UsdDevice* device);
    virtual void observe(UsdBaseObject* caller, UsdDevice* device) {}

  protected:
    virtual bool deferCommit(UsdDevice* device) = 0;  // Returns whether data commit has to be deferred
    virtual bool doCommitData(UsdDevice* device) = 0;
    virtual void doCommitRefs(UsdDevice* device) = 0;

    ANARIDataType type;

    friend class UsdDevice;

    std::vector<UsdBaseObject*> observers;
};

void UsdBridgeAddToCommitList(UsdDevice* device, UsdBaseObject* object, bool commitData); // Helper function to suppress compiler warnings

template<typename T, typename D>
class UsdParameterizedBaseObject : public UsdBaseObject, public UsdParameterizedObject<T, D>
{
  public:
    typedef UsdParameterizedObject<T, D> ParamClass;

    UsdParameterizedBaseObject(ANARIDataType t, UsdDevice* device = nullptr)
      : UsdBaseObject(t, device)
    {}

    virtual ~UsdParameterizedBaseObject()
    {
      if(!claimedUsdName.empty())
        UsdNameRegistryRelease(claimedUsdName, this);
    }

    void filterSetParam(
      const char *name,
      ANARIDataType type,
      const void *mem,
      UsdDevice* device) override
    {
      ParamClass::setParam(name, type, mem, device);
    }

    void filterResetParam(
      const char *name) override
    {
      ParamClass::resetParam(name);
    }

    void resetAllParams() override
    {
      ParamClass::resetParams();
    }

    void* getParameter(const char* name, ANARIDataType& returnType) override
    {
      return ParamClass::getParam(name, returnType);
    }

    int getProperty(const char *name,
      ANARIDataType type,
      void *mem,
      uint64_t size,
      UsdDevice* device) override
    {
      return 0;
    }

    void commit(UsdDevice* device) override
    {
      ParamClass::transferWriteToReadParams();
      UsdBaseObject::commit(device);
    }

    virtual const char* getName() const { return ""; }

  protected:

    void onParamRefChanged(UsdBaseObject* paramObject, bool incRef, bool onWriteParams) override
    {
      if(!onWriteParams && anari::isArray(paramObject->getType()))
      {
        if(incRef)
          paramObject->addObserver(this);
        else
          paramObject->removeObserver(this);
      }
    }

    void observe(UsdBaseObject* caller, UsdDevice* device) override
    {
      if(anari::isArray(caller->getType()))
      {
        UsdBridgeAddToCommitList(device, this, true); // No write to read params; just write to USD
        ParamClass::paramChanged = true;
      }
    }

    bool setNameParam(const char *name,
      ANARIDataType type,
      const void *mem,
      UsdDevice* device)
    {
      const char* objectName = static_cast<const char*>(mem);

      if (type == ANARI_STRING)
      {
        if (strEquals(name, "name"))
        {
          if (!objectName || strEquals(objectName, ""))
          {
            reportStatusThroughDevice(UsdLogInfo(device, this, ANARI_OBJECT, nullptr), ANARI_SEVERITY_WARNING, ANARI_STATUS_NO_ERROR,
              "%s: ANARI object %s cannot be an empty string, using auto-generated name instead.", getName(), "name");
          }
          else
          {
            std::string uniqueName =
              UsdNameRegistryClaim(SanitizeUsdName(objectName), this);

            ParamClass::setParam(name, type, uniqueName.c_str(), device);
            ParamClass::setParam("usd::name", type, uniqueName.c_str(), device);
            this->formatUsdName(this->getWriteParams().usdName);
            claimedUsdName = uniqueName;
          }
          return true;
        }
        else if (strEquals(name, "usd::name"))
        {
          reportStatusThroughDevice(UsdLogInfo(device, this, ANARI_OBJECT, nullptr), ANARI_SEVERITY_WARNING, ANARI_STATUS_NO_ERROR,
            "%s parameter '%s' cannot be set, only read with getProperty().", getName(), "usd::name");
          return true;
        }
      }
      return false;
    }

    int getNameProperty(const char *name,
      ANARIDataType type,
      void *mem,
      uint64_t size,
      UsdDevice* device)
    {
      if (type == ANARI_STRING && strEquals(name, "usd::name"))
      {
        snprintf((char*)mem, size, "%s", UsdSharedString::c_str(this->getReadParams().usdName));
        return 1;
      }
      else if (type == ANARI_UINT64 && strEquals(name, "usd::name.size"))
      {
        if (Assert64bitStringLengthProperty(size, UsdLogInfo(device, this, ANARI_ARRAY, this->getName()), "usd::name.size"))
        {
          uint64_t nameLen = this->getReadParams().usdName ? strlen(this->getReadParams().usdName->c_str())+1 : 0;
          memcpy(mem, &nameLen, size);
        }
        return 1;
      }
      return 0;
    }

    std::string claimedUsdName;
};