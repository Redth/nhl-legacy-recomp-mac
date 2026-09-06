// POSIX stand-ins for the handful of Win32 primitives the offline RE/diagnostic
// headers use (overall_weights_dump.h, stick_list_scan.h, tunable_registry_dump.h,
// tunable_runtime.h). Those headers page-walk guest memory with VirtualQuery to
// avoid faulting on unmapped/no-access pages; macOS exposes the same information
// through mach_vm_region, so they compile and behave unchanged off-Windows.
//
// This is a compatibility shim, not an emulation layer: only the fields and
// constants those call sites actually read are provided.

#pragma once

#ifndef _WIN32

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>

#define MEM_COMMIT 0x1000u
#define MEM_FREE 0x10000u
#define PAGE_NOACCESS 0x001u
#define PAGE_READONLY 0x002u
#define PAGE_READWRITE 0x004u
#define PAGE_GUARD 0x100u

struct MEMORY_BASIC_INFORMATION {
  void* BaseAddress;
  void* AllocationBase;
  unsigned long AllocationProtect;
  std::size_t RegionSize;
  unsigned long State;
  unsigned long Protect;
  unsigned long Type;
};

// Returns sizeof(*out) on success, 0 on failure — matching Win32 so the callers'
// `if (!VirtualQuery(...)) break;` idiom terminates their walks correctly.
inline std::size_t VirtualQuery(const void* address, MEMORY_BASIC_INFORMATION* out,
                                std::size_t length) {
  if (!out || length < sizeof(*out)) return 0;

  const auto query = static_cast<mach_vm_address_t>(reinterpret_cast<uintptr_t>(address));
  mach_vm_address_t region = query;
  mach_vm_size_t region_size = 0;
  vm_region_basic_info_data_64_t info{};
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object = MACH_PORT_NULL;

  // mach_vm_region returns the first region at OR ABOVE the queried address, so
  // a returned base past the query means the query itself sits in a hole.
  const kern_return_t kr =
      mach_vm_region(mach_task_self(), &region, &region_size, VM_REGION_BASIC_INFO_64,
                     reinterpret_cast<vm_region_info_t>(&info), &count, &object);
  if (kr != KERN_SUCCESS) return 0;

  *out = MEMORY_BASIC_INFORMATION{};
  if (region > query) {
    // Unmapped hole up to the next region. Reporting its true length lets the
    // callers' `p += RegionSize` walks skip the gap in one step.
    out->BaseAddress = const_cast<void*>(address);
    out->RegionSize = static_cast<std::size_t>(region - query);
    out->State = MEM_FREE;
    out->Protect = PAGE_NOACCESS;
    return sizeof(*out);
  }

  out->BaseAddress = reinterpret_cast<void*>(static_cast<uintptr_t>(region));
  out->AllocationBase = out->BaseAddress;
  out->RegionSize = static_cast<std::size_t>(region_size);
  out->State = MEM_COMMIT;
  out->Protect = (info.protection & VM_PROT_READ) ? PAGE_READWRITE : PAGE_NOACCESS;
  return sizeof(*out);
}

inline void Sleep(unsigned long milliseconds) {
  ::usleep(static_cast<useconds_t>(milliseconds) * 1000u);
}

// Milliseconds since first call. Callers only use it to timestamp log lines,
// so an arbitrary epoch is fine — the Win32 one is time since boot.
inline unsigned long GetTickCount() {
  static const auto t0 = std::chrono::steady_clock::now();
  return static_cast<unsigned long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - t0)
          .count());
}

#endif  // !_WIN32
