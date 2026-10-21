// nv::（NVN 下面的显卡驱动胶水层：建传输内存、连 nvdrv 服务）。NVN 已整块换成宿主空设备，这层无事可做。
// 分配器回调记在宿主全局 g_nv_alloc，图形层以后要按游戏的分配器分内存时从这里取。
// nv:: (the GPU driver glue layer beneath NVN: creates transfer memory, connects to the nvdrv service). NVN has been replaced wholesale with a host null device, so this layer has nothing to do.
// The allocator callbacks are recorded in the host global g_nv_alloc; the graphics layer takes them from here later when it needs to allocate memory with the game's allocator.
#include "../host/hle.h"

typedef void *(*nv_alloc_fn)(unsigned long size, unsigned long align, void *user);
typedef void (*nv_free_fn)(void *p, void *user);
typedef void *(*nv_realloc_fn)(void *p, unsigned long size, void *user);
struct nv_allocator { nv_alloc_fn alloc; nv_free_fn free; nv_realloc_fn realloc; void *user; };
struct nv_allocator g_nv_alloc, g_nv_devtools_alloc;

#define WHY "NVN 已整块换成宿主空设备，驱动胶水层无事可做"
// 探针仍使用 SDK 媒体组件；其 NvOsAlloc 共用此分配器，必须让原始注册函数执行。
// The probe retains SDK media components whose NvOsAlloc shares this allocator.
#ifndef DM_PROBE_METAL
HLE_FORCE("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", WHY "；分配器记入 g_nv_alloc",
          void, h_nv_SetAlloc, nv_alloc_fn a, nv_free_fn f, nv_realloc_fn r, void *u) { g_nv_alloc = (struct nv_allocator){a, f, r, u}; }
#endif
HLE_FORCE("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", WHY "；分配器记入 g_nv_devtools_alloc",
          void, h_nv_SetDevAlloc, nv_alloc_fn a, nv_free_fn f, nv_realloc_fn r, void *u) { g_nv_devtools_alloc = (struct nv_allocator){a, f, r, u}; }
HLE_FORCE("_ZN2nv18InitializeGraphicsEPvm", WHY, void, h_nv_Init, void *mem, unsigned long size) { (void)mem; (void)size; }
// 探针目标未导入 FinalizeGraphics；保留正式宿主的严格接管完整性检查。
// The probe target does not import FinalizeGraphics; preserve strict force-table checks for the production host.
#ifndef DM_PROBE_METAL
HLE_FORCE("_ZN2nv16FinalizeGraphicsEv", WHY, void, h_nv_Fin, void) {}
#endif

#ifdef DM_PROBE_METAL
// macOS 没有 Switch NVIDIA 设备；返回真实的打开失败，让媒体库处理不可用能力。
// macOS has no Switch NVIDIA device; report open failure to the media library.
HLE_FORCE("NvOsDrvOpen", "native host has no NVIDIA device nodes", int, h_nv_DrvOpen, const char *path) {
    LOG("PROBE NVIDIA device unavailable: %s", path); return -1;
}
#endif
