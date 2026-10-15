// Copyright 2020 The Khronos Group

#include "UsdBaseObject.h"
#include "UsdDevice.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>

namespace
{
struct UsdNameRegistry
{
  std::mutex mutex;
  std::unordered_map<std::string, const void*> claims; // name -> owning object
};

UsdNameRegistry& usdNameRegistry()
{
  static UsdNameRegistry registry;
  return registry;
}
} // namespace

std::string SanitizeUsdName(const char* name)
{
  std::string out = name ? std::string(name) : std::string();

  auto letter = [](unsigned c) { return ((c - 'A') < 26) || ((c - 'a') < 26); };
  auto number = [](unsigned c) { return (c - '0') < 10; };
  auto under  = [](unsigned c) { return c == '_'; };

  for(size_t i = 0; i < out.size(); ++i)
  {
    unsigned x = static_cast<unsigned char>(out[i]);
    if(i == 0)
    {
      if(!letter(x) && !under(x))
        out[i] = '_';
    }
    else if(!letter(x) && !number(x) && !under(x))
      out[i] = '_';
  }
  if(out.empty())
    out = "_";
  return out;
}

std::string UsdNameRegistryClaim(const std::string& desired, const void* owner)
{
  UsdNameRegistry& reg = usdNameRegistry();
  std::lock_guard<std::mutex> lock(reg.mutex);

  for(auto it = reg.claims.begin(); it != reg.claims.end(); )
  {
    if(it->second == owner && it->first != desired)
      it = reg.claims.erase(it);
    else
      ++it;
  }

  auto it = reg.claims.find(desired);
  if(it == reg.claims.end())
  {
    reg.claims.emplace(desired, owner);
    return desired;
  }
  if(it->second == owner)
    return desired; // already claimed by this object (name re-set unchanged)

  std::string candidate;
  unsigned int counter = 1;
  do
  {
    candidate = desired + "_u" + std::to_string(counter++);
    it = reg.claims.find(candidate);
  } while(it != reg.claims.end());

  reg.claims.emplace(candidate, owner);
  return candidate;
}

void UsdNameRegistryRelease(const std::string& claimed, const void* owner)
{
  UsdNameRegistry& reg = usdNameRegistry();
  std::lock_guard<std::mutex> lock(reg.mutex);

  auto it = reg.claims.find(claimed);
  if(it != reg.claims.end() && it->second == owner)
    reg.claims.erase(it);
}

UsdBaseObject::UsdBaseObject(ANARIDataType t, UsdDevice* device)
      : type(t)
{
  if(device)
    device->addToCommitList(this, true);
}

void UsdBaseObject::commit(UsdDevice* device)
{
  bool deferDataCommit = !device->isInitialized() || !device->getReadParams().writeAtCommit || deferCommit(device);
  if(!deferDataCommit)
  {
    bool commitRefs = doCommitData(device);
    if(commitRefs)
      device->addToCommitList(this, false); // Commit refs, but no more data later on
  }
  else
    device->addToCommitList(this, true); // Commit data and refs later on
}

void UsdBaseObject::addObserver(UsdBaseObject* observer)
{
  observers.push_back(observer);
}

void UsdBaseObject::removeObserver(UsdBaseObject* observer)
{
  auto it = std::find(observers.begin(), observers.end(), observer);
  assert(it != observers.end());

  *it = observers.back();
  observers.pop_back();
}

void UsdBaseObject::notify(UsdBaseObject* caller, UsdDevice* device)
{
  auto it = observers.begin();
  while(it != observers.end())
  {
    auto observer = *it;

#ifdef CHECK_MEMLEAKS
    assert(device->isObjAllocated(observer));
#endif

    if(std::find(observers.begin(), it, observer) == it)
      observer->observe(caller, device);

    ++it;
  }
}

void UsdBridgeAddToCommitList(UsdDevice* device, UsdBaseObject* object, bool commitData)
{
  device->addToCommitList(object, commitData);
}
