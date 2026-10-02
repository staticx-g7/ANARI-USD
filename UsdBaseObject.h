// Copyright 2020 The Khronos Group
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "helium/utility/IntrusivePtr.h"
#include "UsdCommonMacros.h"
#include "UsdParameterizedObject.h"

#include <string>

class UsdDevice;

// USD-compatible name sanitization (same rules as UsdParameterizedObject::formatUsdName)
std::string SanitizeUsdName(const char* name);

// Claims a USD object name for the given owner, making it unique within this
// process by appending "_u<n>" if another live object already holds the name.
// Returns the (possibly modified) name that the owner now holds.
std::string UsdNameRegistryClaim(const std::string& desired, const void* owner);

// Releases a previously claimed name (called on object destruction).
void UsdNameRegistryRelease(const std::string& claimed, const void* owner);

// Base parameterized class without being derived as such - nontemplated to allow for polymorphic use
class UsdBaseObject : public helium::RefCounted
{
  public:
    // If device != 0, the object is added to the commit list
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
    virtual bool doCommitData(UsdDevice* device) = 0; // Data commit, execution can be immediate, returns whether doCommitRefs has to be performed
    virtual void doCommitRefs(UsdDevice* device) = 0; // For updates with dependencies on referenced object's data, is always executed deferred

    ANARIDataType type;

    friend class UsdDevice;

    std::vector<UsdBaseObject*> observers;
};

void UsdBridgeAddToCommitList(UsdDevice* device, UsdBaseObject* object, bool commitData); // Helper function to suppress compiler warnings

// Templated base implementation of parameterized object
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

    // Convenience functions for commonly used name property
    virtual const char* getName() const { return ""; }

  protected:

    void onParamRefChanged(UsdBaseObject* paramObject, bool incRef, bool onWriteParams) override
    {
      // Only observe arrays that have actually been committed, so !onWriteParams
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

    // Convenience functions for commonly used name property
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
            // Claim a unique USD name for this object. Producers may hand the
            // same name to two simultaneously live objects (e.g. VTK's ANARI
            // scene graph resets per-actor prop ids before each render while
            // existing nodes keep their cached actor names, so a hidden-then
            // re-shown actor re-assigns an id still owned by a live node).
            // Duplicate names would resolve to the same USD prim and the same
            // clip file, interleaving both actors' arrays into one corrupted
            // mesh. Colliding objects get a "_u<n>" suffix instead; the plain
            // name stays with its first owner while that owner is alive.
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

    // Name currently claimed in the process-wide USD name registry ("" if none)
    std::string claimedUsdName;
};