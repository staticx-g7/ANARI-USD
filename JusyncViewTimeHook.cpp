// JUSYNC ParaView ViewTime bridge.
//
// The VTK ANARI mapper shipped with ParaView 6.1 reads DATA_TIME_STEP from the
// actor's property keys instead of the input data object, so the ANARI
// usd::time parameter is never populated and the sender's device time stays 0.
// ParaView does call vtkPVRenderView::SetViewTime() with the animation time,
// so we hook that exported function and expose the value to the USD device.
//
// This is a deployment workaround; the proper fix is in the VTK ANARI mapper.

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

constexpr size_t kPatchSize = 18; // endbr64 + push r14 + lea rdx + movq r14,xmm0

using SetViewTimeFn = void (*)(void*, double);

std::atomic<double> g_viewTime{std::numeric_limits<double>::quiet_NaN()};
std::atomic<bool> g_installed{false};
std::atomic<bool> g_attempted{false};

unsigned char g_saved[kPatchSize] = {};
void* g_trampoline = nullptr;

void ViewTimeHook(void* self, double time)
{
  if (std::isfinite(time))
    g_viewTime.store(time, std::memory_order_relaxed);
  reinterpret_cast<SetViewTimeFn>(g_trampoline)(self, time);
}

bool InstallHook()
{
  bool expected = false;
  if (!g_attempted.compare_exchange_strong(expected, true))
    return g_installed.load(std::memory_order_acquire);

  void* symbol = dlsym(RTLD_DEFAULT, "_ZN15vtkPVRenderView11SetViewTimeEd");
  if (!symbol)
  {
    // RTLD_NOLOAD only inspects libraries already mapped in the process.
    void* handle = dlopen("libvtkRemotingViews-pv6.1.so.6.1", RTLD_NOW | RTLD_NOLOAD);
    if (!handle)
      handle = dlopen("libvtkRemotingViews-pv6.1.so", RTLD_NOW | RTLD_NOLOAD);
    if (!handle)
    {
      g_attempted.store(false, std::memory_order_release);
      return false;
    }
    symbol = dlsym(handle, "_ZN15vtkPVRenderView11SetViewTimeEd");
    if (!symbol)
    {
      g_attempted.store(false, std::memory_order_release);
      return false;
    }
  }

  // Verify the prologue matches the ParaView 6.1 build we patch. If it does
  // not, leave the binary untouched and let the device fall back to other
  // time sources.
  static const unsigned char expectedPrefix[9] = {
    0xf3, 0x0f, 0x1e, 0xfa, 0x41, 0x56, 0x48, 0x8d, 0x15
  };
  unsigned char* p = reinterpret_cast<unsigned char*>(symbol);
  if (memcmp(p, expectedPrefix, sizeof(expectedPrefix)) != 0)
  {
    std::fprintf(stderr,
      "[JUSYNC] ViewTime hook: unexpected vtkPVRenderView prologue, not patching\n");
    g_attempted.store(false, std::memory_order_release);
    return false;
  }

  memcpy(g_saved, p, kPatchSize);

  g_trampoline = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (g_trampoline == MAP_FAILED)
  {
    g_trampoline = nullptr;
    g_attempted.store(false, std::memory_order_release);
    return false;
  }

  unsigned char* tramp = reinterpret_cast<unsigned char*>(g_trampoline);
  memcpy(tramp, g_saved, kPatchSize);
  tramp[kPatchSize + 0] = 0x48; // movabs rax, <target + kPatchSize>
  tramp[kPatchSize + 1] = 0xb8;
  const uintptr_t resume = reinterpret_cast<uintptr_t>(symbol) + kPatchSize;
  memcpy(tramp + kPatchSize + 2, &resume, sizeof(resume));
  tramp[kPatchSize + 10] = 0xff; // jmp rax
  tramp[kPatchSize + 11] = 0xe0;
  __builtin___clear_cache(reinterpret_cast<char*>(tramp),
    reinterpret_cast<char*>(tramp) + kPatchSize + 12);

  const long pageSize = sysconf(_SC_PAGESIZE);
  const uintptr_t page = reinterpret_cast<uintptr_t>(p) & ~(uintptr_t)(pageSize - 1);
  if (mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(pageSize),
        PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
  {
    std::fprintf(stderr, "[JUSYNC] ViewTime hook: mprotect failed\n");
    g_attempted.store(false, std::memory_order_release);
    return false;
  }

  p[0] = 0x48; // movabs rax, <ViewTimeHook>
  p[1] = 0xb8;
  const uintptr_t hook = reinterpret_cast<uintptr_t>(&ViewTimeHook);
  memcpy(p + 2, &hook, sizeof(hook));
  p[10] = 0xff; // jmp rax
  p[11] = 0xe0;
  memset(p + 12, 0x90, kPatchSize - 12);
  __builtin___clear_cache(reinterpret_cast<char*>(p),
    reinterpret_cast<char*>(p) + kPatchSize);

  g_installed.store(true, std::memory_order_release);
  std::fprintf(stderr, "[JUSYNC] ViewTime hook installed for vtkPVRenderView\n");
  return true;
}

__attribute__((constructor)) void InitViewTimeHook()
{
  InstallHook();
}

} // namespace

extern "C" double JusyncGetHookedViewTime()
{
  if (!g_installed.load(std::memory_order_acquire))
    InstallHook();
  return g_viewTime.load(std::memory_order_relaxed);
}
