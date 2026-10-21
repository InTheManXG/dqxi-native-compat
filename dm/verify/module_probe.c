// 通过真实装载器探测模块与 SDK 启动边界；不链接任何游戏专用 HLE，不把缺失服务返回成功。
// Probe modules and SDK startup through the real loader, without game-specific HLE or successful missing-service stubs.
#include "../host/dm.h"
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <sys/ucontext.h>
#if TARGET_OS_IPHONE
#include <CoreFoundation/CoreFoundation.h>
#endif
#include "probe_input.h"
#ifndef DM_PROBE_METAL
uint64_t probe_keyboard_buttons;
int probe_keyboard_ready, probe_keyboard_modal;
void probe_controller_read(uint64_t state[3]) { memset(state, 0, 3 * sizeof *state); }
#endif

struct dm_flags g_flag;
static unsigned nmissing, ngates;
static pthread_mutex_t probe_lock = PTHREAD_MUTEX_INITIALIZER;
static void probe_unlock(pthread_mutex_t **lock) { pthread_mutex_unlock(*lock); }
#define PROBE_LOCK() pthread_mutex_t *guard __attribute__((cleanup(probe_unlock))) = &probe_lock; pthread_mutex_lock(guard)
static int sm_connected, sm_initialized;
static char sessions[128][9] = {"sm:"};
static unsigned nsessions = 1;
static unsigned session_owner[128];
static int session_open[128] = {1};
static uint64_t save_files[128];
static int is_domain[128], initialized[128];
static uint32_t event_read[128], event_write[128];
static int focus_pending[128], foreground_owned;
static uint32_t request_events[128][4], request_state[128];
static uint32_t self_options[17], terminate_result, fs_access_log_mode;
static int probe_exit_locked;
int probe_can_close(void) { return !__atomic_load_n(&probe_exit_locked, __ATOMIC_ACQUIRE); }
static uint32_t new_session(const char *name) { if (nsessions == 128) die("probe sessions full"); strlcpy(sessions[nsessions], name, 9); session_owner[nsessions] = nsessions; session_open[nsessions] = 1; return 0x8000 + nsessions++; }
static struct { uint64_t ret, orig; int arg; char kind[16]; } gates[1024];
extern void dm_gate(void);

uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
void *hlecount_wrap(const char *kind, const char *name, void *fn) { (void)kind; (void)name; return fn; }
void hlecount_seal(void) {}
// 探针不开诊断采样和画面参数剪口；线程与同步由现有通用层真实执行。
// The probe disables diagnostic sampling and visual parameter cuts; the existing generic layer executes threads and synchronization.
#ifndef DM_PROBE_METAL
uint64_t g_t0_ns;
void crash_thread_init(void) {}
// 无图形探针没有 Metal 纹理页写保护；文件读取使用普通宿主缓冲。
void dm_guest_write_prepare(void *address, size_t size) { (void)address; (void)size; }
#endif
void dm_prof_add(int id, uint64_t ns, uint64_t bytes) { (void)id; (void)ns; (void)bytes; }
uint64_t guest_ns(void) { return now_ns(); }
int dm_detclock_enabled(void) { return 0; }
uint64_t front_game_res(int docked) { (void)docked; return ((uint64_t)720 << 32) | 1280; }
uint64_t dm_game_res(int docked) { return front_game_res(docked); }
int svc_thread(int, uint64_t *);
uint32_t svc_thread_main_init(void);
void guest_thread_init(void);
uint64_t resolve_import(module_t *m, const char *name, int weak) {
    if (weak) { LOG("PROBE weak-zero %s %s", m->name, name); return 0; }
    // 强符号只计数，装载完成后非零即拒绝执行，不将空地址当作成功解析。
    // Count missing strong imports and refuse execution after loading; zero is never treated as a successful resolution.
    LOG("PROBE unresolved %s %s", m->name, name);
    nmissing++;
    return 0;
}
uint64_t unimpl_c(unsigned i, uint64_t *regs) {
    (void)i; (void)regs;
    die("probe unexpected import stub");
}
void gate_register(uint64_t ret, const char *kind, int arg, uint64_t orig) {
    if (ngates == 1024) die("probe gate overflow");
    gates[ngates].ret = ret; gates[ngates].orig = orig; gates[ngates].arg = arg;
    strlcpy(gates[ngates++].kind, kind, sizeof gates[0].kind);
}
// Horizon GetInfo(RandomEntropy) 返回进程固定的四项熵；规则见 Atmosphere kern_svc_info.cpp。
// Horizon GetInfo(RandomEntropy) returns four stable per-process entropy values; see Atmosphere kern_svc_info.cpp.
static uint64_t entropy[4];
static pthread_once_t entropy_once = PTHREAD_ONCE_INIT;
static void entropy_init(void) { arc4random_buf(entropy, sizeof entropy); }
// 沿用宿主的独立虚拟区划分，仅预留地址，不把未映射页伪装成可访问内存。
// Reuse the host's independent virtual-region partition, reserving addresses without making unmapped pages accessible.
static uint8_t *address_space;
static size_t heap_size;
static uint8_t *physical_pages;
static uint64_t clock_mapping;
static uint32_t clock_handle, hid_handle;
static uint64_t hid_mapping;
static uint8_t *hid_writer;
static uint32_t hid_styles, hid_supported;
static uint32_t hid_events[10][2], vibration_devices[32];
static unsigned nvibration;
static uint64_t hid_revision, hid_joy_hold;
static int hid_active;
static uint64_t hid_sample;
// 只有键盘事件源已安装且游戏允许 Pro 手柄时，才发布虚拟手柄连接。
// Connect the virtual pad only after keyboard monitoring is installed and the game permits Pro controllers.
static void hid_tick(void) {
    if (!hid_writer || !hid_active) return;
    uint64_t sample = ++hid_sample;
    int connected = __atomic_load_n(&probe_keyboard_ready, __ATOMIC_ACQUIRE) && (hid_styles & 1) && (hid_supported & 1);
    uint8_t *npad = hid_writer + 0x9a00; uint32_t previous; memcpy(&previous, npad, 4);
    uint32_t style_set = connected ? 1 : 0; memcpy(npad, &style_set, 4);
    uint32_t device = connected ? 1 : 0; memcpy(npad + 0x4188, &device, 4);
    uint64_t properties = connected ? (1ull << 11) | (1ull << 13) | (1ull << 14) | (1ull << 15) : 0;
    memcpy(npad + 0x4190, &properties, 8);
    if (previous != style_set && hid_events[0][0]) { uint64_t event[31] = {hid_events[0][0]}; svc_thread(0x11, event); }
    uint64_t controller[3]; probe_controller_read(controller);
    for (unsigned pad = 0; pad < 10; pad++) for (unsigned style = 0; style < 7; style++) {
        uint64_t *ring = (void *)(hid_writer + 0x9a00 + pad * 0x5000 + 0x28 + style * 0x350);
        uint64_t index = (ring[2] + 1) % 16;
        uint64_t *entry = ring + 4 + index * 6;
        __atomic_store_n(entry, sample, __ATOMIC_SEQ_CST);
        entry[1] = sample; memset(entry + 2, 0, 32);
        if (connected && pad == 0 && (style == 0 || style == 6)) {
            if (!__atomic_load_n(&probe_keyboard_modal, __ATOMIC_ACQUIRE)) {
                uint64_t keyboard = __atomic_load_n(&probe_keyboard_buttons, __ATOMIC_RELAXED);
                probe_combined_input(keyboard, controller, entry + 2);
            }
            entry[5] = 3; // 客体始终看到同一个有线虚拟 Pro 手柄，实体断开后仍可使用键盘。
        }
        ring[1] = 16;
        __atomic_store_n(ring + 2, index, __ATOMIC_SEQ_CST);
        __atomic_store_n(ring + 3, sample < 15 ? sample : 15, __ATOMIC_SEQ_CST);
    }
}
#ifdef DM_PROBE_METAL
static void *hid_worker(void *unused) {
    (void)unused; pthread_setname_np("dqxi-native-input");
    for (;;) { pthread_mutex_lock(&probe_lock); hid_tick(); pthread_mutex_unlock(&probe_lock); usleep(8000); }
    return NULL;
}
#endif
static int romfs_fd = -1, save_root_fd = -1;
static uint64_t romfs_bytes;
static struct { uint64_t dst, src, size; } stack_maps[64];
static unsigned nstack;
static struct { uint64_t address, size; int perm, open; } transfers[16];
static unsigned ntransfer;
static uint32_t bsd_transfer;
static int bsd_monitoring;
static uint8_t bsd_config[32];
static uint32_t performance_config[2];
static int user_parameter_taken, application_running, game_recording_allowed;
static uint32_t copyright_transfer, copyright_width, copyright_height;
static uint32_t audio_parameters[13];
static uint64_t audio_work_size;
static uint32_t audio_event_write, audio_event_read;
static int audio_open;
#ifdef DM_PROBE_METAL
#include "probe_audio.h"
#endif
static int probe_ipc_address(uint64_t *address, uint64_t size);
#include "probe_fs.h"
#include "probe_video.h"
static int copyright_visible;
static uint64_t copyright_address;
static uint64_t image_bytes(void) { uint64_t n = 0; for (int i = 0; i < g_nmod; i++) n += g_mod[i].size + g_mod[i].image_off; return n; }
#ifndef DM_PROBE_COMPACT_AS
#define DM_PROBE_COMPACT_AS TARGET_OS_IPHONE
#endif
#if DM_PROBE_COMPACT_AS
// DQXI 用 VAMM 承载游戏堆；留出 SDK 的前 1 GiB 管理区及 4 GiB 游戏地址区。
#define PROBE_ALIAS_OFF 0ull
#define PROBE_ALIAS_SIZE (0x150000000ull)
#define PROBE_HEAP_CAP 0x08000000ull
#define PROBE_SHARED_CAP 0x08000000ull
#else
#define PROBE_ALIAS_OFF 0x10000000ull
#define PROBE_ALIAS_SIZE (20ull << 30)
#define PROBE_HEAP_CAP (4ull << 30)
#define PROBE_SHARED_CAP (8ull << 30)
#endif
#define PROBE_STACK_OFF (PROBE_ALIAS_OFF + PROBE_ALIAS_SIZE)
#define PROBE_HEAP_OFF (PROBE_STACK_OFF + 0x10000000ull)
#define PROBE_SHARED_OFF (PROBE_HEAP_OFF + PROBE_HEAP_CAP)
#define PROBE_AS_SIZE (PROBE_SHARED_OFF + PROBE_SHARED_CAP)
int probe_memory_prepare(void) {
    if (address_space) return 1;
    void *p;
#if DM_PROBE_COMPACT_AS
    // SDK 对大块按 1 GiB 对齐；对齐预留基址，避免内部空洞吞掉可用的 4 GiB。
    vm_address_t address = 0;
    kern_return_t result = vm_map(mach_task_self(), &address, PROBE_AS_SIZE, (1ull << 30) - 1,
        VM_FLAGS_ANYWHERE, MACH_PORT_NULL, 0, FALSE, VM_PROT_NONE, VM_PROT_ALL, VM_INHERIT_NONE);
    if (result != KERN_SUCCESS) { LOG("PROBE reserve failed bytes=%#llx mach=%d", PROBE_AS_SIZE, result); return 0; }
    p = (void *)address;
#else
    p = mmap(NULL, PROBE_AS_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { LOG("PROBE reserve failed bytes=%#llx errno=%d", PROBE_AS_SIZE, errno); return 0; }
#endif
    physical_pages = calloc(1, PROBE_ALIAS_SIZE >> 12);
    if (!physical_pages) { munmap(p, PROBE_AS_SIZE); return 0; }
    address_space = p;
    LOG("PROBE reserved address space bytes=%#llx alias=%#llx heap-cap=%#llx", PROBE_AS_SIZE, PROBE_ALIAS_SIZE, PROBE_HEAP_CAP);
    return 1;
}
static void memory_init(void) {
    if (!probe_memory_prepare()) die("probe reserve address space");
}
#if DM_PROBE_COMPACT_AS
static uint64_t probe_shared_begin(uint64_t begin, uint64_t end) {
    // 只收窄全地址空间的共享分配器；栈分配器和游戏 VAMM 保留原范围。
    if (address_space && begin == (uint64_t)address_space && end == begin + PROBE_AS_SIZE)
        return begin + PROBE_SHARED_OFF;
    return begin;
}
#endif
// 时区规则在公开 ABI 中是不透明的 16 KiB；宿主保存带版本的本地时区标识，并由同一服务执行转换。
// The public ABI treats a timezone rule as opaque 16 KiB; the host stores a versioned local-zone identity and performs conversion in the same service.
static int local_zone(char name[36]) {
#if TARGET_OS_IPHONE
    // iOS 沙箱不能依赖 /etc/localtime 符号链接；读取系统公开的 IANA 时区标识。
    CFTimeZoneRef zone = CFTimeZoneCopyDefault();
    if (!zone) return 0;
    memset(name, 0, 36);
    int ok = CFStringGetCString(CFTimeZoneGetName(zone), name, 36, kCFStringEncodingUTF8);
    CFRelease(zone);
    return ok;
#else
    char path[1024] = {0}; ssize_t length = readlink("/etc/localtime", path, sizeof path - 1);
    const char *zone = length > 0 ? strstr(path, "/zoneinfo/") : NULL;
    if (!zone || strlen(zone + 10) >= 36) return 0;
    memset(name, 0, 36); memcpy(name, zone + 10, strlen(zone + 10)); return 1;
#endif
}
#ifdef DM_PROBE_METAL
#include "../host/hle.h"
#include <libkern/OSCacheControl.h>
#if DM_PROBE_COMPACT_AS
HLE_FORCE("_ZN2nn2os6detail21AddressSpaceAllocatorC1Emmm", "compact host shared mapping range", void, probe_address_allocator,
          void *allocator, uint64_t begin, uint64_t end, uint64_t guard) {
    void (*original)(void *, uint64_t, uint64_t, uint64_t) = guest_sym("_ZN2nn2os6detail21AddressSpaceAllocatorC1Emmm");
    if (!original) die("probe missing SDK address allocator");
    // 沿用 SDK 的锁、随机选址及保护页，避免 64 次随机重试耗尽后时钟初始化中止。
    original(allocator, probe_shared_begin(begin, end), end, guard);
}
#endif
// 平台缓存维护必须通过 macOS API；直接执行 Switch CTR_EL0/DC CIVAC 会触发 SIGILL。
// Platform cache maintenance must use macOS APIs; guest CTR_EL0/DC CIVAC traps with SIGILL.
HLE_FORCE("_ZN2nn2os14FlushDataCacheEPKvm", "macOS platform cache maintenance", void, probe_flush_data_cache, const void *address, size_t size) {
    sys_dcache_flush((void *)address, size);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}
#endif
#if defined(DM_PROBE_METAL) || defined(DM_PROBE_CAPTURE_TEST)
#include <CommonCrypto/CommonDigest.h>
void probe_shader_dump(const void *control, uint64_t address) {
    const char *directory = getenv("DM_PROBE_SHADER_DUMP"); if (!directory) return;
    uint8_t ctl[0x880], digest[32]; vm_size_t got;
    if (vm_read_overwrite(mach_task_self(), (vm_address_t)control, sizeof ctl, (vm_address_t)ctl, &got) != KERN_SUCCESS || got != sizeof ctl)
        die("probe shader control unreadable");
    uint32_t magic, size, constant_offset; memcpy(&magic, ctl, 4); memcpy(&size, ctl + 0x704, 4); memcpy(&constant_offset, ctl + 0x700, 4);
    if (magic != 0x98761234 || constant_offset > size) die("probe shader invalid control header");
    if (!size || size > (1u << 20)) die("probe shader size invalid %u", size);
    uint8_t *code = malloc(size); if (!code) die("probe shader allocation");
    if (vm_read_overwrite(mach_task_self(), address, size, (vm_address_t)code, &got) != KERN_SUCCESS || got != size)
        die("probe shader code unreadable");
    CC_SHA256(ctl, sizeof ctl, digest); char key[17];
    for (int k = 0; k < 8; k++) snprintf(key + 2 * k, 3, "%02x", digest[k]);
    char path[4096];
    for (int part = 0; part < 2; part++) {
        if (snprintf(path, sizeof path, "%s/%s.%s", directory, key, part ? "code" : "control") >= sizeof path) die("probe shader path too long");
        int fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW, 0600);
        if (fd < 0) { if (errno == EEXIST) continue; die("probe shader output open"); }
        const uint8_t *data = part ? code : ctl; size_t bytes = part ? size : sizeof ctl, offset = 0;
        while (offset < bytes) { ssize_t n = write(fd, data + offset, bytes - offset); if (n < 0 && errno == EINTR) continue; if (n <= 0) die("probe shader output write"); offset += n; }
        if (close(fd)) die("probe shader output close");
    }
    free(code); LOG("PROBE shader %s bytes=%u", key, size);
}
#endif
// SDK 在 IPC 描述符中只保留低 39 位；只对本探针拥有的客体地址区恢复高位。
static int probe_ipc_address(uint64_t *address, uint64_t size) {
    const uint64_t span = 1ull << 39, base = (uint64_t)address_space;
    if (!base) return 1;
    uint64_t candidate = (base & ~(span - 1)) | *address;
    if (candidate < base) candidate += span;
    if (candidate - base >= PROBE_AS_SIZE) return 1;
    if (size > PROBE_AS_SIZE - (candidate - base)) return 0;
    if (candidate != *address) {
        // 模块常量和早期构造函数的宿主栈仍用原地址，重叠时拒绝猜测。
        uint64_t stack_end = (uint64_t)pthread_get_stackaddr_np(pthread_self());
        uint64_t stack_start = stack_end - pthread_get_stacksize_np(pthread_self());
        if (module_of((void *)*address) || (*address >= stack_start && *address < stack_end)) {
            vm_address_t mapped = candidate; vm_size_t length;
            vm_region_basic_info_data_64_t info; mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t object = MACH_PORT_NULL;
            kern_return_t result = vm_region_64(mach_task_self(), &mapped, &length, VM_REGION_BASIC_INFO_64,
                (vm_region_info_t)&info, &count, &object);
            if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
            // 只有另一候选也实际可访问才有歧义；未提交预留区不能遮蔽合法宿主指针。
            return result != KERN_SUCCESS || mapped > candidate || info.protection == VM_PROT_NONE;
        }
        *address = candidate;
    }
    return 1;
}
static int ipc_buffer(const uint32_t *desc, uint64_t *address, uint64_t *size) {
    if (desc[2] & 0x00ffffe0) return 0;
    *size = desc[0] | ((uint64_t)((desc[2] >> 24) & 15) << 32);
    *address = desc[1] | ((uint64_t)(desc[2] >> 28) << 32) | ((uint64_t)((desc[2] >> 2) & 7) << 36);
    return probe_ipc_address(address, *size) && *address < (1ull << 48) && *size <= (1ull << 48) - *address;
}
uint64_t gate_dispatch(uint64_t ret, uint64_t *regs) {
    for (unsigned i = 0; i < ngates; i++) if (gates[i].ret == ret) {
        if (!strcmp(gates[i].kind, "cntpct") || !strcmp(gates[i].kind, "cntvct")) {
            if (gates[i].arg < 0 || gates[i].arg > 30) goto unsupported;
            uint64_t ns = now_ns(), tick = (ns / 625) * 12 + (ns % 625) * 12 / 625;
            regs[gates[i].arg] = tick; return tick;
        }
        if (!strcmp(gates[i].kind, "svc") && gates[i].arg == 0x1f) {
            module_t *owner = module_of((void *)regs[1]);
            if (owner && regs[1] + 4 <= (uint64_t)owner->base + owner->size && !memcmp((void *)regs[1], "sm:", 4)) {
                if (sm_connected) goto unsupported;
                sm_connected = 1;
                LOG("PROBE connect service-manager sm:");
                regs[0] = 0; regs[1] = 0x8000; return 0;
            }
        }
        if (!strcmp(gates[i].kind, "svc") && gates[i].arg == 0x15) {
            PROBE_LOCK();
            uint64_t addr = regs[1], size = regs[2]; module_t *owner = module_of((void *)addr);
            if (!size || (addr & 0xfff) || (size & 0xfff) || regs[3] > 3 || ntransfer == 16) goto unsupported;
            int valid = owner && addr >= (uint64_t)owner->base + owner->data_off && size <= (uint64_t)owner->base + owner->size - addr;
            uint64_t alias = (uint64_t)address_space + PROBE_ALIAS_OFF;
            if (address_space && addr >= alias && size <= PROBE_ALIAS_SIZE && addr - alias <= PROBE_ALIAS_SIZE - size) {
                valid = 1;
                for (size_t k = (addr - alias) >> 12; k < (addr - alias + size) >> 12; k++) if (physical_pages[k] != 1) valid = 0;
            }
            if (!valid) goto unsupported;
            // 原生服务直接持有客体映射；未实现跨进程映射前不伪造另一份缓冲。
            // Native services retain the guest mapping; do not invent a second buffer before cross-process mapping exists.
            transfers[ntransfer] = (typeof(transfers[0])){addr, size, (int)regs[3], 1};
            regs[1] = 0x9000 + ntransfer++; regs[0] = 0; return 0;
        }
        if (!strcmp(gates[i].kind, "svc") && gates[i].arg == 0x16 && regs[0] >= 0x9000 && regs[0] - 0x9000 < ntransfer) {
            PROBE_LOCK();
            unsigned transfer = regs[0] - 0x9000;
            if (!transfers[transfer].open) goto unsupported;
            transfers[transfer].open = 0; regs[0] = 0; return 0;
        }
        if (!strcmp(gates[i].kind, "svc") && gates[i].arg == 0x16 &&
                regs[0] >= 0x8000 && regs[0] - 0x8000 < nsessions && session_open[regs[0] - 0x8000]) {
            PROBE_LOCK();
            session_open[regs[0] - 0x8000] = 0;
            unsigned root = session_owner[regs[0] - 0x8000], references = 0;
            for (unsigned s=0;s<nsessions;s++) references += session_open[s] && session_owner[s]==root;
            if (!references && save_files[root]) { h_fs_CloseFile(save_files[root]); save_files[root]=0; }
            regs[0] = 0; return 0;
        }
        if (!strcmp(gates[i].kind, "svc") && gates[i].arg == 0x21) {
            PROBE_LOCK();
            uint32_t *ipc = pthread_getspecific(g_keytlr);
            // HIPC 描述符和特殊头都计入 CMIF 对齐；所有字段必须留在 256 字节命令区。
            // Include HIPC descriptors and special headers in CMIF alignment; all fields must fit the 256-byte command buffer.
            if (ipc && regs[0] >= 0x8000 && regs[0] - 0x8000 < nsessions && sm_connected && session_open[regs[0] - 0x8000]) {
                unsigned type = ipc[0] & 0xffff, words = ipc[1] & 0x3ff, off = 2;
                if (type == 2) { regs[0] = 0xf601; return 0; }
                if (ipc[1] & 0x80000000) {
                    unsigned special = ipc[off++];
                    if (special & ~0x1ffu) goto unsupported;
                    off += (special & 1 ? 2 : 0) + ((special >> 1) & 15) + ((special >> 5) & 15);
                }
                unsigned statics = (ipc[0] >> 16) & 15, sends = (ipc[0] >> 20) & 15;
                unsigned recvs = (ipc[0] >> 24) & 15, exchanges = (ipc[0] >> 28) & 15;
                unsigned buffer_word = off + 2 * statics;
                off = buffer_word + 3 * (sends + recvs + exchanges);
                if (off > 64) goto unsupported;
                unsigned aligned = (off + 3) & ~3u, root = session_owner[regs[0] - 0x8000], object = root;
                int domain = is_domain[root] && (type == 4 || type == 6);
                if (off + words > 64 || aligned + 4 > off + words) goto unsupported;
                if (domain) {
                    unsigned id = ipc[aligned + 1];
                    if (!id || id > nsessions || session_owner[id - 1] != root || !sessions[id - 1][0]) goto unsupported;
                    if ((ipc[aligned] & 0xffff) == 2) {
                        if (save_files[id-1]) { h_fs_CloseFile(save_files[id-1]); save_files[id-1]=0; }
                        sessions[id - 1][0] = 0; regs[0] = 0; return 0;
                    }
                    if ((ipc[aligned] & 0xffff) != 1) goto unsupported;
                    object = id - 1; aligned += 4;
                }
                if (aligned + 4 > off + words || ipc[aligned] != 0x49434653) goto unsupported;
                if (type != 4 && type != 5 && type != 6 && type != 7) goto unsupported;
                unsigned cmd = ipc[aligned + 2], token = ipc[aligned + 3];
                if (type == 5 || type == 7) { if (cmd != 0 && cmd != 2 && cmd != 3) goto unsupported; }
                const char *service = sessions[object];
                uint32_t moved = 0, copied = 0, copied_second = 0, result = 0; uint64_t value = 0;
                uint8_t payload[64] = {0}; unsigned payload_size = 8;
                LOG("PROBE IPC %s type=%u command=%u domain=%d", service, type, cmd, domain);
                if ((type == 5 || type == 7) && cmd == 0 && !is_domain[root]) {
                    is_domain[root] = 1; value = root + 1;
                } else if ((type == 5 || type == 7) && cmd == 2) {
                    moved = new_session(service);
                    session_owner[moved - 0x8000] = root;
                } else if (!strcmp(service, "sm:") && (type == 4 || type == 6) && cmd == 0 && !sm_initialized) {
                    sm_initialized = 1;
                } else if ((type == 5 || type == 7) && cmd == 3) {
                    // HID 列表和文件系统路径使用指针描述符；容量必须覆盖完整路径。
                    // Pointer capacity must fit HID arrays and full filesystem paths.
                    value = !strcmp(service, "hid") ? 0x100 : !strcmp(service, "fsp-srv") || !strcmp(service, "savefs") ? 0x1000 : 0;
                } else if (!strcmp(service, "sm:") && (type == 4 || type == 6) && cmd == 1 &&
                           sm_initialized && aligned + 6 <= off + words) {
                    char name[9] = {0}; memcpy(name, ipc + aligned + 4, 8);
                    const char *available[] = {"hid", "audren:u", "nim:eca", "acc:u0", "friend:u", "ssl", "bsd:u", "nifm:u", "time:u", "fsp-srv", "lm", "apm", "appletOE", "aoc:u", "pctl", "pctl:s", "pctl:a", "pctl:r"};
                    int found = 0;
                    for (unsigned k = 0; k < sizeof available / sizeof *available; k++)
                        if (!strcmp(name, available[k])) found = 1;
                    if (!found) { LOG("PROBE requested service %s", name); goto unsupported; }
                    moved = new_session(name);
                } else if (!strcmp(service, "hid") && cmd == 0 && aligned + 6 <= off + words) {
                    moved = new_session("hidres");
                } else if (!strcmp(service, "hid") && cmd == 109 && aligned + 8 <= off + words && hid_writer) {
                    memcpy(&hid_revision, ipc + aligned + 4, 8); hid_active = 1;
                    for (int k = 0; k < 17; k++) hid_tick();
#ifdef DM_PROBE_METAL
                    static int started;
                    if (!started) { pthread_t worker; if (pthread_create(&worker, NULL, hid_worker, NULL)) goto unsupported; pthread_detach(worker); started = 1; }
#endif
                } else if (!strcmp(service, "hid") && cmd == 100 && aligned + 8 <= off + words) {
                    hid_styles = ipc[aligned + 4]; hid_tick();
                } else if (!strcmp(service, "hid") && cmd == 102 && statics == 1 && !sends && !recvs && !exchanges) {
                    const uint32_t *desc = ipc + buffer_word - 2;
                    uint64_t address = desc[1] | (((uint64_t)desc[0] << 20) & 0x0f00000000ull) | (((uint64_t)desc[0] << 30) & 0x7000000000ull);
                    uint32_t bytes = desc[0] >> 16, ids[10]; vm_size_t got = 0;
                    if ((desc[0] & 0xe3f) || bytes > sizeof ids || bytes % 4 || !probe_ipc_address(&address, bytes) ||
                        (bytes && (vm_read_overwrite(mach_task_self(), address, bytes, (vm_address_t)ids, &got) != KERN_SUCCESS || got != bytes))) goto unsupported;
                    uint32_t supported = 0;
                    for (unsigned k = 0; k < bytes / 4; k++) {
                        unsigned index = ids[k] < 8 ? ids[k] : ids[k] == 32 ? 8 : ids[k] == 16 ? 9 : 10;
                        if (index == 10) goto unsupported;
                        supported |= 1u << index;
                    }
                    // 首次连接必须在返回前发布，不能依赖抢锁中的后台采样线程。
                    hid_supported = supported; hid_tick();
                } else if (!strcmp(service, "hid") && cmd == 106 && aligned + 10 <= off + words) {
                    unsigned id = ipc[aligned + 4], index = id < 8 ? id : id == 32 ? 8 : id == 16 ? 9 : 10;
                    if (index == 10) goto unsupported;
                    if (!hid_events[index][0]) {
                        uint64_t event[31] = {0}; if (!svc_thread(0x45, event) || event[0]) goto unsupported;
                        hid_events[index][0] = event[1]; hid_events[index][1] = event[2];
                    }
                    uint64_t event[31] = {hid_events[index][0]}; if (!svc_thread(0x11, event) || event[0]) goto unsupported;
                    copied = hid_events[index][1];
                } else if (!strcmp(service, "hid") && cmd == 101) {
                    value = hid_styles;
                } else if (!strcmp(service, "hid") && cmd == 200 && aligned + 5 <= off + words) {
                    uint32_t handle = ipc[aligned + 4]; unsigned k = 0;
                    for (; k < nvibration; k++) if (vibration_devices[k] == handle) break;
                    if (k == nvibration) goto unsupported;
                    // 当前没有振动硬件，明确报告 DeviceType=None、Position=None。
                    // No vibration hardware is attached: DeviceType=None, Position=None.
                    value = 0;
                } else if (!strcmp(service, "hid") && cmd == 201 && aligned + 12 <= off + words && !statics && !sends && !recvs && !exchanges) {
                    uint32_t handle = ipc[aligned + 4]; unsigned k = 0;
                    for (; k < nvibration; k++) if (vibration_devices[k] == handle) break;
                    if (k == nvibration) goto unsupported;
                    // 完整请求为句柄、四个浮点数、对齐和用户 ID；已报告 None 的键盘设备没有马达可驱动。
                    // Consume handle, four floats, padding and user ID; the advertised None device has no motor.
                    payload_size = 0;
                } else if (!strcmp(service, "hid") && cmd == 120 && aligned + 8 <= off + words) {
                    uint64_t hold; memcpy(&hold, ipc + aligned + 6, 8);
                    if (hold > 1) goto unsupported;
                    hid_joy_hold = hold;
                } else if (!strcmp(service, "hid") && cmd == 121) {
                    value = hid_joy_hold;
                } else if (!strcmp(service, "hid") && cmd == 203) {
                    moved = new_session("viblist");
                } else if (!strcmp(service, "viblist") && cmd == 0 && aligned + 5 <= off + words) {
                    uint32_t handle = ipc[aligned + 4]; unsigned k = 0;
                    for (; k < nvibration; k++) if (vibration_devices[k] == handle) break;
                    if (k == nvibration) { if (nvibration == 32) goto unsupported; vibration_devices[nvibration++] = handle; }
                } else if (!strcmp(service, "hidres") && cmd == 0) {
                    if (!hid_handle) hid_handle = new_session("hidmem");
                    copied = hid_handle;
                } else if (!strcmp(service, "audren:u")) {
                    if (cmd == 1) {
                        if (aligned + 17 > off + words) goto unsupported;
                        const uint32_t *p = ipc + aligned + 4;
                        // 仅接受已验证的 REV7、48 kHz/240 帧配置；空间按宿主混音状态实际预算。
                        // Accept verified REV7 at 48 kHz/240 frames; budget host mixer state explicitly.
                        if (p[0] != 48000 || p[1] != 240 || !p[2] || p[2] > 24 || p[3] > 255 ||
                            !p[4] || p[4] > 1024 || !p[5] || p[5] > 256 || p[6] > 256 ||
                            p[7] || p[8] || p[9] || p[10] || p[11] || p[12] != 0x37564552) goto unsupported;
                        memcpy(audio_parameters, p, sizeof audio_parameters);
                        #ifdef DM_PROBE_METAL
                        if (!probe_audio_load()) goto unsupported;
                        audio_work_size = probe_audio_size(p);
                        if (!audio_work_size || audio_work_size > (64ull << 20)) goto unsupported;
#else
                        goto unsupported;
#endif
                        value = audio_work_size;
                    } else if (cmd == 0) {
                        if (audio_open || !(ipc[1] & 0x80000000) || ipc[2] != 5 || aligned + 24 > off + words ||
                            memcmp(ipc + aligned + 4, audio_parameters, sizeof audio_parameters)) goto unsupported;
                        uint32_t handle = ipc[5]; uint64_t bytes; memcpy(&bytes, ipc + aligned + 18, 8);
                        if (handle < 0x9000 || handle - 0x9000 >= ntransfer || !transfers[handle - 0x9000].open ||
                            bytes != audio_work_size || transfers[handle - 0x9000].size < bytes) goto unsupported;
                        uint64_t event[31] = {0}; if (!svc_thread(0x45, event) || event[0]) goto unsupported;
                        audio_event_write = event[1]; audio_event_read = event[2];
#ifdef DM_PROBE_METAL
                        result = probe_audio_open(audio_parameters, transfers[handle - 0x9000].address, bytes);
                        if (result) goto unsupported;
#else
                        goto unsupported;
#endif
                        audio_open = 1;
                        moved = new_session("audr");
                    } else if (cmd == 4 && aligned + 8 <= off + words && ipc[aligned + 4] == 0x37564552) {
                        moved = new_session("auddev");
                    } else goto unsupported;
                } else if (!strcmp(service, "auddev") && cmd == 10 && !sends && recvs == 1 && !exchanges) {
                    uint64_t address, bytes;
                    if (!ipc_buffer(ipc + buffer_word, &address, &bytes) || bytes < 256) goto unsupported;
                    // 与原生音频渲染器固定的虚拟 TV 输出身份一致；名称是协议名，不是 Mac 设备名。
                    // Match the native renderer's virtual TV endpoint; this is a protocol name, not a Mac device name.
                    char name[256] = "AudioTvOutput";
                    if (vm_write(mach_task_self(), address, (vm_offset_t)name, sizeof name) != KERN_SUCCESS) goto unsupported;
                } else if (((!strcmp(service, "auddev") && cmd == 12) || (!strcmp(service, "aoc:u") && cmd == 8))) {
                    // 固定输出和只读 DLC 集合均不会变更；返回真实未触发事件。
                    // Fixed output and read-only DLC membership never change; return a real unsignaled event.
                    if (!event_read[object]) {
                        uint64_t event[31] = {0}; if (!svc_thread(0x45, event) || event[0]) goto unsupported;
                        event_write[object] = event[1]; event_read[object] = event[2];
                    }
                    copied = event_read[object];
                } else if (!strcmp(service, "auddev") && cmd == 5) {
                    // 原生输出桥仅支持双声道，与 Driver.SupportsChannelCount 保持一致。
                    // The native output bridge supports stereo only, matching Driver.SupportsChannelCount.
                    value = 2;
                } else if (!strcmp(service, "audr") && cmd == 7 && audio_open) {
                    copied = audio_event_read;
                } else if (!strcmp(service, "audr")) {
#ifdef DM_PROBE_METAL
                    if ((cmd == 4 || cmd == 10) && sends == 1 && recvs == 2 && !exchanges) {
                        uint64_t address[3], bytes[3];
                        for (int k = 0; k < 3; k++) if (!ipc_buffer(ipc + buffer_word + 3 * k, &address[k], &bytes[k]) || bytes[k] > 0x100000 ||
                            !probe_audio_validate(address[k], bytes[k], k != 0)) goto unsupported;
                        result = probe_audio_update((void *)address[0], bytes[0], (void *)address[1], bytes[1], (void *)address[2], bytes[2]);
                        if (result) { LOG("PROBE audio update error %#x", result); goto unsupported; }
                    } else if (cmd <= 9 && cmd != 4 && cmd != 7) {
                        if (cmd == 8 && aligned + 5 > off + words) goto unsupported;
                        uint32_t out = 0;
                        result = probe_audio_control(cmd, cmd == 8 ? ipc[aligned + 4] : 0, &out); value = out;
                        if (result) goto unsupported;
                    } else {
#endif
                    for (unsigned k = aligned + 4; k < off + words; k++) LOG("PROBE audio argument %u=%#x", k - aligned - 4, ipc[k]);
                    LOG("PROBE audio buffers sends=%u receives=%u exchanges=%u", sends, recvs, exchanges);
                    const char *directory = getenv("DM_PROBE_SHADER_DUMP");
                    if (directory && sends == 1) {
                        uint64_t address, bytes; vm_size_t got;
                        if (!ipc_buffer(ipc + buffer_word, &address, &bytes) || bytes > 0x100000) goto unsupported;
                        uint8_t *data = malloc(bytes); if (!data) die("audio capture allocation");
                        if (vm_read_overwrite(mach_task_self(), address, bytes, (vm_address_t)data, &got) != KERN_SUCCESS || got != bytes) goto unsupported;
                        char path[4096]; snprintf(path, sizeof path, "%s/audio-update.bin", directory);
                        int fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW, 0600);
                        if (fd >= 0) { if (write(fd, data, bytes) != bytes || close(fd)) die("audio capture write"); }
                        else if (errno != EEXIST) die("audio capture open");
                        free(data);
                    }
                    goto unsupported;
#ifdef DM_PROBE_METAL
                    }
#endif
                } else if (!strcmp(service, "fsp-srv") && cmd == 1 && (type == 4 || type == 6)) {
                    if (!(ipc[1] & 0x80000000) || !(ipc[2] & 1)) goto unsupported;
                    initialized[object] = 1;
                } else if (!strcmp(service, "fsp-srv") && initialized[object] && cmd == 51) {
                    if (aligned + 22 > off + words || save_root_fd < 0) goto unsupported;
                    const uint8_t *attr = (const void *)(ipc + aligned + 4);

                    uint64_t uid[2] = {(uint64_t)getuid() + 1, 0x444d4c4f43414c};
                    // 当前只挂载本地用户的主存档；其他类型必须显式实现，不能混用同一目录。
                    // Only the local user's primary account save may use this mount.
                    if (attr[0] != 1 || memcmp(attr + 16, uid, 16) || attr[40] != 1 || attr[41] || attr[42] || attr[43]) goto unsupported;
                    for (unsigned k = 1; k < 8; k++) if (attr[k]) goto unsupported;
                    for (unsigned k = 32; k < 40; k++) if (attr[k]) goto unsupported;
                    for (unsigned k = 44; k < 72; k++) if (attr[k]) goto unsupported;
                    uint64_t program; memcpy(&program, attr + 8, 8);
                    // main.npdm 的 ACI0 标识已核对；体验版等其它程序的存档不在本挂载内。
                    // Verified against main.npdm ACI0; other titles (including demos) have no save here.
                    if (program && program != UINT64_C(0x01006c300e9f0000)) {
                        LOG("PROBE save for other program %#llx not found", program); result = (1002u << 9) | 2u;
                    } else {
                        if (!probe_save_mount()) goto unsupported;
                        moved = new_session("savefs");
                    }
                } else if (!strcmp(service, "fsp-srv") && initialized[object] && cmd == 200) {
                    if (romfs_fd < 0) {
                        const char *path = getenv("DM_PROBE_ROMFS"); struct stat st;
                        if (!path || (romfs_fd = open(path, O_RDONLY | O_NOFOLLOW)) < 0 || fstat(romfs_fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0x50)
                            goto unsupported;
                        uint64_t header;
                        if (pread(romfs_fd, &header, 8, 0) != 8 || header != 0x50) die("probe invalid RomFS storage header");
                        romfs_bytes = st.st_size;
                    }
                    moved = new_session("romstore");
                } else if (!strcmp(service, "savefs") && cmd == 10) {
                    result = h_fs_Commit("save");
                } else if (!strcmp(service, "savefs") && statics == 1 && !sends && !recvs && !exchanges) {
                    char path[0x310];
                    if (!probe_save_path(ipc + buffer_word - 2, path)) goto unsupported;
                    LOG("PROBE save path %s", path);
                    if (cmd == 7) { int32_t entry = 0; result = h_fs_GetEntryType(&entry, path); value = entry; }
                    else if (cmd == 2) result = h_fs_CreateDirectory(path);
                    else if (cmd == 1) result = h_fs_DeleteFile(path);
                    else if (cmd == 8 && aligned + 5 <= off + words) {
                        unsigned mode=ipc[aligned+4];uint64_t file=0;
                        if (!(mode&3) || (mode&~7u) || ((mode&4)&&!(mode&2))) goto unsupported;
                        result=h_fs_OpenFile(&file,path,mode);
                        if (!result) { moved=new_session("savefile");save_files[moved-0x8000]=file; }
                    }
                    else if (cmd == 0 && aligned + 8 <= off + words && !ipc[aligned + 4]) {
                        int64_t size; memcpy(&size, ipc + aligned + 6, 8);
                        if (size < 0) goto unsupported;
                        result = h_fs_CreateFile(path, size);
                    } else goto unsupported;
                } else if (!strcmp(service, "savefile") && save_files[object]) {
                    uint64_t file=save_files[object];
                    if (statics || exchanges) goto unsupported;
                    if (cmd==0 || cmd==1) {
                        uint64_t address,bytes,length;int64_t offset;
                        if (aligned+10>off+words || sends!=(cmd==1) || recvs!=(cmd==0) ||
                            !ipc_buffer(ipc+buffer_word,&address,&bytes)) goto unsupported;
                        unsigned option=ipc[aligned+4];
                        memcpy(&offset,ipc+aligned+6,8);memcpy(&length,ipc+aligned+8,8);
                        if (option>(cmd==1) || offset<0 || length>bytes || length>INT64_MAX-(uint64_t)offset) goto unsupported;
                        if (!(h_fs_GetFileOpenMode(file)&(cmd==1?MODE_WRITE:MODE_READ))) goto unsupported;
                        // 经宿主缓冲复制，坏客体地址不能让文件后端越界；任何半写失败都阻止事务提交。
                        // 门调用沿用客体的小栈，传输缓冲必须放在线程本地存储。
                        static _Thread_local uint8_t chunk[0x10000];uint64_t done=0;
                        while (done<length) {
                            size_t n=length-done<sizeof chunk?length-done:sizeof chunk, got=0;
                            if (cmd==1) {
                                vm_size_t copied_bytes=0;
                                if (vm_read_overwrite(mach_task_self(),address+done,n,(vm_address_t)chunk,&copied_bytes)!=KERN_SUCCESS || copied_bytes!=n) { save_poison(EFAULT);goto unsupported; }
                                result=h_fs_WriteFile(file,offset+done,chunk,n,NULL);got=n;
                            } else {
                                result=h_fs_ReadFile(&got,file,offset+done,chunk,n);
                                if (!result && got && vm_write(mach_task_self(),address+done,(vm_offset_t)chunk,got)!=KERN_SUCCESS) goto unsupported;
                            }
                            if (result) break;
                            done+=got;if (got<n) break;
                        }
                        value=done;
                        if (cmd==1 && option && !result && fsync((int)file)) { save_poison(errno);result=err_res("fsync","savefile"); }
                    } else if (sends || recvs) goto unsupported;
                    else if (cmd==2) { if (fsync((int)file)) { save_poison(errno);result=err_res("fsync","savefile"); } }
                    else if (cmd==3 && aligned+6<=off+words) {
                        int64_t size;memcpy(&size,ipc+aligned+4,8);
                        if (size<0 || !(h_fs_GetFileOpenMode(file)&MODE_WRITE)) goto unsupported;
                        result=h_fs_SetFileSize(file,size);
                    } else if (cmd==4) { int64_t size=0;result=h_fs_GetFileSize(&size,file);value=size; }
                    else goto unsupported;
                } else if (!strcmp(service, "romstore") && cmd == 4) {
                    value = romfs_bytes;
                } else if (!strcmp(service, "romstore") && cmd == 0 && recvs == 1 && !sends && !exchanges) {
                    uint64_t address, bytes, offset, length;
                    if (aligned + 8 > off + words || !ipc_buffer(ipc + buffer_word, &address, &bytes)) goto unsupported;
                    memcpy(&offset, ipc + aligned + 4, 8); memcpy(&length, ipc + aligned + 6, 8);
                    if (offset > romfs_bytes || length > romfs_bytes - offset || length > bytes) goto unsupported;
                    static _Thread_local uint8_t chunk[0x10000]; uint64_t done = 0;
                    while (done < length) {
                        size_t n = length - done < sizeof chunk ? length - done : sizeof chunk;
                        ssize_t read = pread(romfs_fd, chunk, n, offset + done);
                        if (read < 0 && errno == EINTR) continue;
                        if (read <= 0 || vm_write(mach_task_self(), address + done, (vm_offset_t)chunk, read) != KERN_SUCCESS) goto unsupported;
                        done += read;
                    }
                    LOG("PROBE RomFS read offset=%#llx size=%#llx", offset, length);
                } else if (!strcmp(service, "fsp-srv") && initialized[object] && cmd == 1005) {
                    value = fs_access_log_mode;
                } else if (!strcmp(service, "fsp-srv") && initialized[object] && cmd == 1004) {
                    if (aligned + 5 > off + words || ipc[aligned + 4] != 0) goto unsupported;
                    fs_access_log_mode = ipc[aligned + 4];
                } else if (!strcmp(service, "nim:eca") && cmd == 0) {
                    if (!(ipc[1] & 0x80000000) || ipc[2] != 3 || ipc[5] < 0x9000 || ipc[5] - 0x9000 >= ntransfer || !transfers[ipc[5] - 0x9000].open) goto unsupported;
                    initialized[object] = ipc[5]; moved = new_session("shopserv");
                } else if (!strcmp(service, "acc:u0") && (cmd == 100 || cmd == 140)) {
                    if (!(ipc[1] & 0x80000000) || !(ipc[2] & 1)) goto unsupported;
                    initialized[object] = 1;
                } else if (!strcmp(service, "acc:u0") && cmd == 1 && aligned + 8 <= off + words) {
                    uint64_t uid[2] = {(uint64_t)getuid() + 1, 0x444d4c4f43414c};
                    value = !memcmp(ipc + aligned + 4, uid, sizeof uid);
                } else if (!strcmp(service, "acc:u0") && cmd == 101 && aligned + 8 <= off + words) {
                    uint64_t uid[2] = {(uint64_t)getuid() + 1, 0x444d4c4f43414c};
                    if (memcmp(ipc + aligned + 4, uid, sizeof uid)) { LOG("PROBE requested unknown local user"); result = (100 << 9) | 124; }
                    else moved = new_session("baas");
                } else if (!strcmp(service, "baas") && (cmd == 0 || cmd == 1)) {
                    // 本地身份没有在线账户授权；不能生成假的网络账户 ID 或令牌。
                    // The local identity has no online authorization; never invent a network ID or token.
                    result = (59 << 9) | 124;
                } else if (!strcmp(service, "acc:u0") && (cmd == 0 || cmd == 150)) {
                    value = 1;
                } else if (!strcmp(service, "acc:u0") && (cmd == 4 || cmd == 51)) {
                    // 使用宿主用户派生的稳定离线身份，只供本地存档；不是 Nintendo 在线账号。
                    // Use a stable host-user-derived offline identity for local saves, not a Nintendo online account.
                    uint64_t uid[2] = {(uint64_t)getuid() + 1, 0x444d4c4f43414c};
                    if (cmd == 51 && aligned + 5 > off + words) goto unsupported;
                    if (cmd == 51 && ipc[aligned + 4]) memset(uid, 0, sizeof uid);
                    memcpy(payload, uid, sizeof uid); payload_size = sizeof uid;
                } else if (!strcmp(service, "friend:u") && cmd == 0) {
                    moved = new_session("friends");
                } else if (!strcmp(service, "ssl") && cmd == 5) {
                    if (aligned + 5 > off + words || ipc[aligned + 4] > 3) goto unsupported;
                    initialized[object] = ipc[aligned + 4] + 1;
                } else if (!strcmp(service, "bsd:u") && cmd == 0) {
                    if (!(ipc[1] & 0x80000000) || ipc[2] != 3 || aligned + 16 > off + words) goto unsupported;
                    uint32_t handle = ipc[5]; uint64_t bytes;
                    memcpy(&bytes, ipc + aligned + 14, 8);
                    if (handle < 0x9000 || handle - 0x9000 >= ntransfer || !transfers[handle - 0x9000].open ||
                        bytes != transfers[handle - 0x9000].size) goto unsupported;
                    memcpy(bsd_config, ipc + aligned + 4, sizeof bsd_config);
                    bsd_transfer = handle; initialized[object] = 1; value = 0;
                } else if (!strcmp(service, "bsd:u") && cmd == 1 && bsd_transfer) {
                    if (aligned + 6 > off + words) goto unsupported;
                    uint64_t pid; memcpy(&pid, ipc + aligned + 4, 8);
                    if (pid != 0 && pid != (uint64_t)getpid()) goto unsupported;
                    bsd_monitoring = 1;
                } else if (!strcmp(service, "bsd:u") && cmd == 2 && bsd_monitoring) {
                    // 无网络桥接时 Socket 明确失败，不产生可用套接字。
                    // Without a network bridge Socket explicitly fails and produces no usable socket.
                    value = ((uint64_t)50 << 32) | UINT32_MAX;
                } else if (!strcmp(service, "nifm:u") && (cmd == 4 || cmd == 5)) {
                    moved = new_session("nifmgen");
                } else if (!strcmp(service, "nifmgen") && cmd == 4) {
                    moved = new_session("netreq"); request_state[moved - 0x8000] = 1;
                } else if (!strcmp(service, "netreq") && cmd == 2) {
                    if (!request_events[object][0]) for (unsigned k = 0; k < 2; k++) {
                        uint64_t r[31] = {0};
                        if (!svc_thread(0x45, r) || r[0]) goto unsupported;
                        request_events[object][k * 2] = r[1]; request_events[object][k * 2 + 1] = r[2];
                    }
                    copied = request_events[object][1]; copied_second = request_events[object][3];
                } else if (!strcmp(service, "netreq") && cmd == 0) {
                    value = request_state[object];
                } else if (!strcmp(service, "netreq") && cmd == 1) {
                    result = 110 | (311 << 9);
                } else if (!strcmp(service, "netreq") && (cmd == 3 || cmd == 4)) {
                    request_state[object] = 1;
                    uint64_t r[31] = {request_events[object][0]};
                    if (!r[0] || !svc_thread(0x11, r) || r[0]) goto unsupported;
                } else if (!strcmp(service, "nifmgen") && (cmd == 18 || cmd == 21 || cmd == 22)) {
                    // 探针没有网络请求或接口桥接，明确报告未连接。
                    // The probe has no network requests or interface bridge; explicitly report disconnected.
                    value = 0;
                } else if (!strcmp(service, "aoc:u") && (cmd == 2 || cmd == 3) && (type == 4 || type == 6)) {
                    // 当前只装载本体与更新存储；未挂载的 DLC 不应出现在内容枚举中。
                    // Only base/update storage is mounted; unmounted DLC must not appear in the content enumeration.
                    value = 0;
                } else if (!strcmp(service, "timezone") && cmd == 0) {
                    if (!local_zone((char *)payload)) goto unsupported;
                    payload_size = 36;
                } else if (!strcmp(service, "timezone") && cmd == 4 && recvs == 1 && !sends && !exchanges) {
                    uint64_t address, bytes; char zone[36];
                    static _Thread_local uint8_t rule[0x4000]; memset(rule, 0, sizeof rule);
                    if (aligned + 13 > off + words || !local_zone(zone) || memcmp(ipc + aligned + 4, zone, 36) ||
                        !ipc_buffer(ipc + buffer_word, &address, &bytes) || bytes != sizeof rule) goto unsupported;
                    memcpy(rule, "DM-TZ-1", 8); memcpy(rule + 8, zone, sizeof zone);
                    if (vm_write(mach_task_self(), address, (vm_offset_t)rule, sizeof rule) != KERN_SUCCESS) goto unsupported;
                } else if (!strcmp(service, "timezone") && (cmd == 100 || cmd == 101)) {
                    if (aligned + 6 > off + words) goto unsupported;
                    if (cmd == 100) {
                        uint64_t address, bytes; uint8_t rule[44]; char zone[36]; vm_size_t got;
                        if (sends != 1 || recvs || exchanges || !ipc_buffer(ipc + buffer_word, &address, &bytes) || bytes != 0x4000 ||
                            vm_read_overwrite(mach_task_self(), address, sizeof rule, (vm_address_t)rule, &got) != KERN_SUCCESS || got != sizeof rule ||
                            memcmp(rule, "DM-TZ-1", 8) || !local_zone(zone) || memcmp(rule + 8, zone, 36)) goto unsupported;
                    }
                    time_t timestamp; struct tm calendar;
                    memcpy(&timestamp, ipc + aligned + 4, 8);
                    if (!localtime_r(&timestamp, &calendar) || calendar.tm_year < -1900 || calendar.tm_year > 30867) goto unsupported;
                    int16_t year = calendar.tm_year + 1900; memcpy(payload, &year, 2);
                    payload[2] = calendar.tm_mon + 1; payload[3] = calendar.tm_mday; payload[4] = calendar.tm_hour;
                    payload[5] = calendar.tm_min; payload[6] = calendar.tm_sec;
                    uint32_t day = calendar.tm_wday, ordinal = calendar.tm_yday, dst = calendar.tm_isdst > 0;
                    int32_t offset = calendar.tm_gmtoff;
                    memcpy(payload + 8, &day, 4); memcpy(payload + 12, &ordinal, 4);
                    if (calendar.tm_zone) strlcpy((char *)payload + 16, calendar.tm_zone, 8);
                    memcpy(payload + 24, &dst, 4); memcpy(payload + 28, &offset, 4); payload_size = 32;
                } else if (!strcmp(service, "time:u") && cmd == 20) {
                    if (!clock_handle) clock_handle = new_session("clockmem");
                    copied = clock_handle;
                } else if (!strcmp(service, "time:u") && cmd <= 4) {
                    const char *clocks[] = {"usrclock", "netclock", "steady", "timezone", "locclock"};
                    moved = new_session(clocks[cmd]);
                } else if ((!strcmp(service, "usrclock") || !strcmp(service, "locclock")) && cmd == 0) {
                    value = (uint64_t)time(NULL);
                } else if (!strcmp(service, "lm") && (type == 4 || type == 6) && cmd == 0) {
                    moved = new_session("logger");
                } else if (!strcmp(service, "apm") && (type == 4 || type == 6) && cmd == 0) {
                    moved = new_session("apmsess");
                } else if (!strcmp(service, "apmsess") && cmd == 0) {
                    if (aligned + 6 > off + words || ipc[aligned + 4] > 1) goto unsupported;
                    performance_config[ipc[aligned + 4]] = ipc[aligned + 5];
                    if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0)) goto unsupported;
                } else if (!strcmp(service, "apmsess") && cmd == 1) {
                    if (aligned + 5 > off + words || ipc[aligned + 4] > 1) goto unsupported;
                    value = performance_config[ipc[aligned + 4]];
                } else if (!strcmp(service, "appletOE") && (type == 4 || type == 6) && cmd == 0) {
                    moved = new_session("appproxy");
                } else if (!strcmp(service, "appproxy") && (type == 4 || type == 6)) {
                    const char *name = cmd == 0 ? "state" : cmd == 1 ? "self" : cmd == 2 ? "window" :
                        cmd == 3 ? "audio" : cmd == 4 ? "display" : cmd == 11 ? "creator" : cmd == 20 ? "appfunc" : cmd == 1000 ? "debug" : NULL;
                    if (!name) goto unsupported;
                    moved = new_session(name);
                } else if (!strcmp(service, "window") && (type == 4 || type == 6) && cmd == 1) {
                    value = (uint64_t)getpid();
                } else if ((!strcmp(service, "self") && cmd == 91) || (!strcmp(service, "state") && cmd == 0)) {
                    if (!event_read[object]) {
                        uint64_t event_regs[31] = {0};
                        if (!svc_thread(0x45, event_regs) || event_regs[0]) goto unsupported;
                        event_write[object] = event_regs[1]; event_read[object] = event_regs[2];
                        if (!strcmp(service, "state")) {
                            focus_pending[object] = 1;
                            event_regs[0] = event_write[object]; svc_thread(0x11, event_regs);
                        }
                    }
                    copied = event_read[object];
                } else if (!strcmp(service, "state") && cmd == 1) {
                    if (focus_pending[object]) {
                        value = 15; focus_pending[object] = 0;
                        uint64_t clear[31] = {event_read[object]}; svc_thread(0x17, clear);
                    } else result = 0x680;
                } else if (!strcmp(service, "state") && cmd == 66) {
                    if (aligned + 5 > off + words) goto unsupported;
                    LOG("PROBE CPU boost mode=%u", ipc[aligned + 4]);
                    if (ipc[aligned + 4] > 2) goto unsupported;
                    // 将加载性能提示映射为当前游戏线程 QoS，不宣称能锁定 Mac CPU 频率。
                    // Map the loading hint to game-thread QoS; this does not lock the Mac CPU frequency.
                    int qos_result = pthread_set_qos_class_self_np(ipc[aligned + 4] ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_USER_INITIATED, 0);
                    if (qos_result) { LOG("PROBE CPU boost QoS error=%d", qos_result); goto unsupported; }
                } else if (!strcmp(service, "state") && (cmd == 5 || cmd == 6 || cmd == 9)) {
                    // 底座运行模式与 Boost 性能模式保持一致；焦点状态仍为前台（1）。
                    value = 1;
                } else if (!strcmp(service, "window") && cmd == 10) {
                    foreground_owned = 1;
                } else if (!strcmp(service, "self") && (cmd == 1 || cmd == 2)) {
                    // ISelfController LockExit/UnlockExit：存档临界区内禁止窗口和菜单退出。
                    if (statics || sends || recvs || exchanges) goto unsupported;
                    __atomic_store_n(&probe_exit_locked, cmd == 1, __ATOMIC_RELEASE);
                } else if (!strcmp(service, "self") && (cmd == 10 || cmd == 11 || cmd == 12 || cmd == 13 || cmd == 14 || cmd == 16)) {
                    if (aligned + 5 > off + words) goto unsupported;
                    self_options[cmd] = ipc[aligned + 4];
                } else if ((!strcmp(service, "pctl") || !strcmp(service, "pctl:s") || !strcmp(service, "pctl:a") || !strcmp(service, "pctl:r")) && (cmd == 0 || cmd == 1)) {
                    moved = new_session("pctlobj");
                } else if (!strcmp(service, "pctlobj") && cmd == 1) {
                    initialized[object] = 1;
                } else if (!strcmp(service, "appfunc") && cmd == 20 && aligned + 8 <= off + words) {
                    uint64_t uid[2] = {(uint64_t)getuid() + 1, 0x444d4c4f43414c};
                    if (memcmp(ipc + aligned + 4, uid, sizeof uid)) goto unsupported;
                    if (save_root_fd < 0) {
                        const char *directory = getenv("DM_PROBE_SAVE_DIR");
                        if (!directory || directory[0] != '/') goto unsupported;
                        if (mkdir(directory, 0700) && errno != EEXIST) goto unsupported;
                        save_root_fd = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
                        if (save_root_fd < 0) goto unsupported;
                        // 只创建探针独立存档目录；不读取或迁移其它游戏/模拟器存档。
                        // Create only the probe's isolated save directory; never migrate other saves.
                        LOG("PROBE isolated save directory ready");
                    }
                    value = 0;
                } else if (!strcmp(service, "appfunc") && cmd == 67 && aligned + 5 <= off + words) {
                    // 这是录制许可设置，不是开始录像；没有录制后端时不宣称支持录像。
                    // This configures recording permission, not capture; no recording capability is advertised.
                    game_recording_allowed = ipc[aligned + 4] != 0;
                } else if (!strcmp(service, "appfunc") && cmd == 1) {
                    if (aligned + 5 > off + words) goto unsupported;
                    if (ipc[aligned + 4] != 2 || user_parameter_taken) result = 0x680;
                    else { moved = new_session("userparm"); user_parameter_taken = 1; }
                } else if (!strcmp(service, "userparm") && cmd == 0) {
                    moved = new_session("userdata");
                } else if (!strcmp(service, "userdata") && cmd == 0) {
                    value = 0x88;
                } else if (!strcmp(service, "userdata") && cmd == 11 && recvs == 1 && !sends && !exchanges) {
                    uint64_t address, bytes, offset; uint8_t data[0x88] = {0};
                    if (aligned + 6 > off + words || !ipc_buffer(ipc + buffer_word, &address, &bytes)) goto unsupported;
                    memcpy(&offset, ipc + aligned + 4, 8);
                    if (offset > sizeof data || bytes > sizeof data - offset) goto unsupported;
                    uint32_t magic = 0xc79497ca; memcpy(data, &magic, 4); data[4] = 1;
                    uint64_t uid[2] = {(uint64_t)getuid() + 1, 0x444d4c4f43414c}; memcpy(data + 8, uid, 16);
                    if (vm_write(mach_task_self(), address, (vm_offset_t)(data + offset), bytes) != KERN_SUCCESS) goto unsupported;
                } else if (!strcmp(service, "appfunc") && cmd == 100) {
                    if (!(ipc[1] & 0x80000000) || ipc[2] != 2 || aligned + 8 > off + words) goto unsupported;
                    uint32_t handle = ipc[3], w = ipc[aligned + 4], h = ipc[aligned + 5]; uint64_t bytes;
                    memcpy(&bytes, ipc + aligned + 6, 8);
                    if (handle < 0x9000 || handle - 0x9000 >= ntransfer || !transfers[handle - 0x9000].open || !w || !h ||
                        w > 4096 || h > 4096 || bytes != transfers[handle - 0x9000].size || (uint64_t)w * h * 4 > bytes) goto unsupported;
                    copyright_transfer = handle; copyright_width = w; copyright_height = h;
                    // IPC 复制句柄后服务独立持有映射；关闭发送方句柄不能撤销服务的引用。
                    // The IPC copied handle retains the mapping independently of the sender handle.
                    copyright_address = transfers[handle - 0x9000].address;
                } else if (!strcmp(service, "appfunc") && cmd == 101 && copyright_transfer && sends == 1 && !recvs && !exchanges) {
                    uint64_t address, bytes; vm_size_t got;
                    if (aligned + 9 > off + words || !ipc_buffer(ipc + buffer_word, &address, &bytes)) goto unsupported;
                    uint32_t x = ipc[aligned + 4], y = ipc[aligned + 5], w = ipc[aligned + 6], h = ipc[aligned + 7], origin = ipc[aligned + 8];
                    if (x > copyright_width || w > copyright_width - x || y > copyright_height || h > copyright_height - y ||
                        origin > 1 || bytes < (uint64_t)w * h * 4) goto unsupported;
                    uint64_t target = copyright_address;
                    for (uint32_t row = 0; row < h; row++) {
                        uint32_t out_row = origin ? y + row : copyright_height - 1 - y - row;
                        if (vm_read_overwrite(mach_task_self(), address + (uint64_t)row * w * 4, w * 4,
                            target + ((uint64_t)out_row * copyright_width + x) * 4, &got) != KERN_SUCCESS || got != w * 4) goto unsupported;
                    }
                } else if (!strcmp(service, "appfunc") && cmd == 102 && copyright_transfer) {
                    if (aligned + 5 > off + words || ipc[aligned + 4] > 1) goto unsupported;
                    copyright_visible = ipc[aligned + 4];
                } else if (!strcmp(service, "appfunc") && cmd == 40) {
                    application_running = 1; value = 1;
                } else if (!strcmp(service, "appfunc") && cmd == 21) {
                    // 本次目标 Mac 的首选语言实测为 zh-Hans-CN，对应 Horizon 简体中文代码。
                    // The target Mac preference is verified as zh-Hans-CN, mapped to Horizon's Simplified Chinese code.
                    memcpy(&value, "zh-Hans", 8);
                } else if (!strcmp(service, "appfunc") && cmd == 22) {
                    if (aligned + 5 > off + words) goto unsupported;
                    terminate_result = ipc[aligned + 4];
                } else if (!strcmp(service, "logger") && cmd == 0 && sends == 1 && !recvs && !exchanges) {
                    uint64_t address, bytes;
                    if (!ipc_buffer(ipc + buffer_word, &address, &bytes)) goto unsupported;
                    static _Thread_local char packet[4097]; vm_size_t got = 0;
                    if (bytes > 4096 || vm_read_overwrite(mach_task_self(), address, bytes, (vm_address_t)packet, &got) != KERN_SUCCESS || got != bytes)
                        goto unsupported;
                    for (uint64_t k = 0; k < bytes; k++) if ((unsigned char)packet[k] < 32 || (unsigned char)packet[k] > 126) packet[k] = '.';
                    packet[bytes] = 0; LOG("GUEST LOG %s", packet);
                } else {
                    if (!strcmp(service, "sm:") && (type == 4 || type == 6) && cmd == 1 && aligned + 6 <= off + words)
                        LOG("PROBE requested service %.8s", (char *)(ipc + aligned + 4));
                    goto unsupported;
                }
                if (payload_size == 8) memcpy(payload, &value, 8);
                memset(ipc, 0, 256);
                unsigned response_off = 2;
                if (copied || (!domain && moved)) {
                    ipc[1] = 0x80000000; response_off = 3;
                    ipc[2] = copied ? (copied_second ? 4 : 2) : 0x20;
                    ipc[response_off++] = copied ? copied : moved;
                    if (copied_second) ipc[response_off++] = copied_second;
                }
                unsigned response = (response_off + 3) & ~3u;
                if (domain) { ipc[response] = moved ? 1 : 0; response += 4; }
                ipc[response] = 0x4f434653; ipc[response + 2] = result; ipc[response + 3] = token;
                memcpy(ipc + response + 4, payload, payload_size);
                unsigned end = response + 4 + (payload_size + 3) / 4;
                if (domain && moved) {
                    session_owner[moved - 0x8000] = root;
                    ipc[response + 4] = moved - 0x8000 + 1;
                }
                // HIPC 原始区包含最多三字对齐余量；两个句柄会把 CMIF 推到下一组 16 字节。
                // HIPC raw data includes alignment slack; two handles move CMIF to the next 16-byte boundary.
                ipc[1] |= end - response_off + 2;
                regs[0] = 0; return 0;
            }

        }
        if (!strcmp(gates[i].kind, "svc") && svc_thread(gates[i].arg, regs)) return 0;
        if (!strcmp(gates[i].kind, "svc") && gates[i].arg == 0x29 &&
                regs[1] == 11 && regs[2] == 0 && regs[3] < 4) {
            pthread_once(&entropy_once, entropy_init);
            // SVC 的输出在 x1，由 SDK 包装器写回 out；不能在此解引用原 x0。
            // The SDK wrapper stores the x1 result to out; never dereference the incoming x0 here.
            regs[1] = entropy[regs[3]]; regs[0] = 0;
            LOG("PROBE GetInfo RandomEntropy index=%llu OK", regs[3]);
            return 0;
        }
        if (!strcmp(gates[i].kind, "svc") && (gates[i].arg == 4 || gates[i].arg == 5)) {
            PROBE_LOCK();
            memory_init();
            uint64_t dst = regs[0], src = regs[1], size = regs[2];
            uint64_t stack = (uint64_t)address_space + PROBE_STACK_OFF, alias = (uint64_t)address_space + PROBE_ALIAS_OFF;
            if (!size || (dst & 0xfff) || (src & 0xfff) || (size & 0xfff) ||
                dst < stack || size > 0x10000000 || dst - stack > 0x10000000 - size || src > UINT64_MAX - size) goto unsupported;
            int alias_source = src >= alias && size <= PROBE_ALIAS_SIZE && src - alias <= PROBE_ALIAS_SIZE - size;
            module_t *source = module_of((void *)src);
            // 模块静态线程栈只能来自原始可写数据区，不能包含代码、键页或补丁。
            // Static module stacks must come from original writable data, excluding code, key pages and patches.
            if (!alias_source && (!source || src < (uint64_t)source->base + source->data_off ||
                src + size > (uint64_t)source->base + source->key_page_off)) goto unsupported;
            uint64_t lo = dst & ~0x3fffull, hi = (dst + size + 0x3fff) & ~0x3fffull;
            if ((clock_mapping && lo < clock_mapping + 0x4000 && clock_mapping < hi) ||
                (hid_mapping && lo < hid_mapping + 0x40000 && hid_mapping < hi)) goto unsupported;
            size_t first = alias_source ? (src - alias) >> 12 : 0, pages = size >> 12;
            if (gates[i].arg == 4) {
                if (nstack == 64) goto unsupported;
                for (unsigned k = 0; k < nstack; k++)
                    if (lo < ((stack_maps[k].dst + stack_maps[k].size + 0x3fff) & ~0x3fffull) && (stack_maps[k].dst & ~0x3fffull) < hi) goto unsupported;
                for (unsigned k = 0; k < nstack; k++)
                    if (src < stack_maps[k].src + stack_maps[k].size && stack_maps[k].src < src + size) goto unsupported;
                if (alias_source) {
                    for (size_t k = first; k < first + pages; k++) if (physical_pages[k] != 1) goto unsupported;
                } else {
                    for (uint64_t cursor = src; cursor < src + size;) {
                        vm_address_t at = cursor; vm_size_t length;
                        vm_region_basic_info_data_64_t protection;
                        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t port;
                        kern_return_t result = vm_region_64(mach_task_self(), &at, &length, VM_REGION_BASIC_INFO_64,
                            (vm_region_info_t)&protection, &count, &port);
                        if (result != KERN_SUCCESS) goto unsupported;
                        if (port) mach_port_deallocate(mach_task_self(), port);
                        if (at > cursor || !length || (protection.protection & 7) != (VM_PROT_READ | VM_PROT_WRITE)) goto unsupported;
                        cursor = at + length;
                    }
                }
                if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE)) goto unsupported;
                memcpy((void *)dst, (void *)src, size);
                // shortcut: 共享宿主页的 4 KiB 源区只作逻辑锁定；需要精确访存隔离时升级为地址转换。
                // shortcut: source subpages sharing a host page are locked logically; use address translation when exact access isolation is required.
                if (!(src & 0x3fff) && !(size & 0x3fff) && mprotect((void *)src, size, PROT_NONE)) die("probe protect moved stack source");
                if (alias_source) memset(physical_pages + first, 3, pages);
                stack_maps[nstack++] = (typeof(stack_maps[0])){dst, src, size};
            } else {
                unsigned k = 0;
                for (; k < nstack; k++) if (stack_maps[k].dst == dst && stack_maps[k].src == src && stack_maps[k].size == size) break;
                if (k == nstack) goto unsupported;
                if (!(src & 0x3fff) && !(size & 0x3fff) && mprotect((void *)src, size, PROT_READ | PROT_WRITE)) goto unsupported;
                memcpy((void *)src, (void *)dst, size);
                if (mprotect((void *)lo, hi - lo, PROT_NONE) || madvise((void *)lo, hi - lo, MADV_DONTNEED)) die("probe release stack mapping");
                if (alias_source) memset(physical_pages + first, 1, pages);
                stack_maps[k] = stack_maps[--nstack];
            }
            LOG("PROBE stack map %#x destination=%#llx size=%#llx", gates[i].arg, dst, size);
            regs[0] = 0; return 0;
        }
        if (!strcmp(gates[i].kind, "svc") && gates[i].arg == 0x13 && ((clock_handle && regs[0] == clock_handle) || (hid_handle && regs[0] == hid_handle))) {
            PROBE_LOCK();
            memory_init();
            uint64_t addr = regs[1];
            int hid = hid_handle && regs[0] == hid_handle;
            uint64_t bytes = hid ? 0x40000 : 0x1000, allocation = hid ? bytes : 0x4000;
            uint64_t *mapping = hid ? &hid_mapping : &clock_mapping;
            // SDK 在整个预留区选择地址；不得覆盖已提交堆或别名页。
            // The SDK chooses an address in the whole reservation; committed heap and alias pages must not be overwritten.
            if (*mapping || regs[2] != bytes || regs[3] != 1 || (addr & 0x3fff) ||
                addr < (uint64_t)address_space || addr - (uint64_t)address_space > PROBE_AS_SIZE - allocation ||
                (clock_mapping && addr < clock_mapping + 0x4000 && clock_mapping < addr + allocation) ||
                (hid_mapping && addr < hid_mapping + 0x40000 && hid_mapping < addr + allocation)) goto unsupported;
            for (unsigned k = 0; k < nstack; k++)
                if (addr < stack_maps[k].dst + stack_maps[k].size && stack_maps[k].dst < addr + allocation) goto unsupported;
            // 独立共享区避免 SDK 随机映射占用游戏随后线性提交的 alias/heap/stack 区。
            // A dedicated shared region avoids SDK random mappings consuming later linear game allocations.
            if (addr < (uint64_t)address_space + PROBE_SHARED_OFF) { regs[0] = 0xdc01; return 0; }
            if (mprotect((void *)addr, allocation, PROT_READ | PROT_WRITE)) goto unsupported;
            // 未接入输入前不虚报连接手柄；零采样数表示没有可用输入样本。
            // Until input is connected, zero sample counts report no input rather than inventing a controller.
            memset((void *)addr, 0, allocation);
            if (hid) {
                // 服务写入视图与客体只读视图共享同一存储，不能临时把客体权限改成可写。
                // Separate service-write and guest-read views share storage without relaxing guest permissions.
                // iOS 的 /tmp 不属于应用沙箱，使用宿主入口提供的临时目录。
                const char *temporary = getenv("TMPDIR"); if (!temporary) temporary = "/tmp";
                char path[4096];
                if (snprintf(path, sizeof path, "%s/dqxi-hid-XXXXXX", temporary) >= sizeof path) die("probe HID path too long");
                int fd = mkstemp(path);
                if (fd < 0) die("probe HID backing"); unlink(path);
                if (ftruncate(fd, bytes)) die("probe HID size");
                hid_writer = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                if (hid_writer == MAP_FAILED || mmap((void *)addr, bytes, PROT_READ, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) die("probe HID mapping");
                close(fd);
                *mapping = addr; regs[0] = 0; LOG("PROBE HID shared memory mapped %#llx", addr); return 0;
            }
            uint8_t *page = (void *)addr, id[16]; arc4random_buf(id, sizeof id);
            memset(page, 0, 0x4000);
            int64_t monotonic = now_ns() / 1000000000, offset = (int64_t)time(NULL) - monotonic;
            for (unsigned copy = 0; copy < 2; copy++) {
                memcpy(page + 8 + copy * 24 + 8, id, sizeof id);
                memcpy(page + 0x40 + copy * 32, &offset, 8);
                memcpy(page + 0x48 + copy * 32, &monotonic, 8);
                memcpy(page + 0x50 + copy * 32, id, sizeof id);
            }
            // 网络校时时钟保持未初始化，不能把宿主墙钟冒充网络验证时间。
            // Leave the network clock uninitialized; host wall time is not network-validated time.
            if (mprotect((void *)addr, 0x4000, PROT_READ)) die("probe protect clock page");
            clock_mapping = addr; regs[0] = 0;
            LOG("PROBE shared clock mapped %#llx", addr); return 0;
        }
        if (!strcmp(gates[i].kind, "svc") && (gates[i].arg == 0x2c || gates[i].arg == 0x2d)) {
            PROBE_LOCK();
            memory_init();
            uint64_t base = (uint64_t)address_space + PROBE_ALIAS_OFF, addr = regs[0], size = regs[1];
            if (!size || (addr & 0xfff) || (size & 0xfff) || addr < base || size > PROBE_ALIAS_SIZE || addr - base > PROBE_ALIAS_SIZE - size)
                goto unsupported;
            size_t first = (addr - base) >> 12, count = size >> 12;
            for (size_t k = first; k < first + count; k++) if (physical_pages[k] >= 2) goto unsupported;
            uint64_t host_lo = addr & ~0x3fffull, host_hi = (addr + size + 0x3fff) & ~0x3fffull;
            if (clock_mapping && host_lo < clock_mapping + 0x4000 && clock_mapping < host_hi) goto unsupported;
            if (gates[i].arg == 0x2c) {
                uint64_t lo = addr & ~0x3fffull, hi = (addr + size + 0x3fff) & ~0x3fffull;
                if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE)) { regs[0] = 0xd401; return 0; }
                for (size_t k = first; k < first + count; k++) if (!physical_pages[k]) memset((void *)(base + (k << 12)), 0, 0x1000);
                memset(physical_pages + first, 1, count);
            } else {
                memset(physical_pages + first, 0, count);
                // macOS 16 KiB 页仍有其他 4 KiB 客体页在用时，不能撤销整页权限。
                // Do not revoke a macOS 16 KiB page while another guest 4 KiB subpage still uses it.
                for (size_t k = first & ~3ul; k < first + count; k += 4)
                    if (!(physical_pages[k] | physical_pages[k+1] | physical_pages[k+2] | physical_pages[k+3])) {
                        void *page = (void *)(base + (k << 12));
                        if (mprotect(page, 0x4000, PROT_NONE) || madvise(page, 0x4000, MADV_DONTNEED)) die("probe unmap physical page");
                    }
            }
            LOG("PROBE physical %#x address=%#llx size=%#llx OK", gates[i].arg, addr, size);
            regs[0] = 0; return 0;
        }
        if (!strcmp(gates[i].kind, "svc") && gates[i].arg == 1) {
            PROBE_LOCK();
            memory_init();
            uint64_t size = regs[1], cap = PROBE_SHARED_OFF - PROBE_HEAP_OFF;
            if ((size & 0x1fffff) || size > cap) { regs[0] = 0xca01; return 0; }
            if (size > (4ull << 30) - image_bytes()) { regs[0] = 0x10801; return 0; }
            uint8_t *heap = address_space + PROBE_HEAP_OFF;
            if (clock_mapping && clock_mapping < (uint64_t)heap + size && (uint64_t)heap < clock_mapping + 0x4000) { regs[0] = 0xdc01; return 0; }
            if (size > heap_size && mprotect(heap + heap_size, size - heap_size, PROT_READ | PROT_WRITE)) { regs[0] = 0xd401; return 0; }
            if (size < heap_size) {
                if (mprotect(heap + size, heap_size - size, PROT_NONE)) { regs[0] = 0xd401; return 0; }
                if (madvise(heap + size, heap_size - size, MADV_DONTNEED)) die("probe discard heap");
            }
            heap_size = size; regs[0] = 0; regs[1] = (uint64_t)heap;
            LOG("PROBE SetHeapSize %#llx OK", size); return 0;
        }
        if (!strcmp(gates[i].kind, "svc") && gates[i].arg == 6) {
            PROBE_LOCK();
            uint64_t top = (uint64_t)pthread_get_stackaddr_np(pthread_self());
            uint64_t size = pthread_get_stacksize_np(pthread_self()), bottom = top - size;
            // 输出须落在宿主栈或已登记客体栈，未知地址按实际客体映射边界返回空闲。
            // Output must fit a host or registered guest stack; unknown addresses report holes bounded by guest mappings.
            module_t *mapped = module_of((void *)regs[2]);
            int output_ok = regs[0] >= bottom && regs[0] <= top - 40;
            for (unsigned k = 0; k < nstack; k++)
                if (regs[0] >= stack_maps[k].dst && regs[0] - stack_maps[k].dst <= stack_maps[k].size - 40) output_ok = 1;
            if (regs[2] >= (1ull << 48) || !output_ok)
                goto unsupported;
            struct { uint64_t base, size; uint32_t state, attr, perm, ipc, device, pad; } info =
                {bottom, size, 0xb, 0, 3, 0, 0, 0};
            if (mapped) {
                vm_address_t at = regs[2]; vm_size_t length;
                vm_region_basic_info_data_64_t protection;
                mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t port;
                if (vm_region_64(mach_task_self(), &at, &length, VM_REGION_BASIC_INFO_64,
                                 (vm_region_info_t)&protection, &count, &port) != KERN_SUCCESS || at > regs[2]) goto unsupported;
                if (port) mach_port_deallocate(mach_task_self(), port);
                // SDK 从 Code/CodeData 区间反查 MOD0；宿主跳板必须是独立 Static 区，不能冒充 NSO 头。
                // The SDK discovers MOD0 through Code/CodeData regions; host trampolines are separate Static regions, not NSO headers.
                uint64_t base = (uint64_t)mapped->base, lo, hi;
                if (regs[2] < base) {
                    lo = base - mapped->image_off; hi = base; info.state = 2;
                } else if (regs[2] < base + mapped->data_off) {
                    lo = base; hi = base + mapped->data_off; info.state = 3;
                } else if (regs[2] < base + mapped->key_page_off + mapped->key_page_size) {
                    lo = base + mapped->data_off; hi = base + mapped->key_page_off + mapped->key_page_size; info.state = 4;
                } else {
                    lo = base + mapped->key_page_off + mapped->key_page_size; hi = base + mapped->size; info.state = 2;
                }
                info.base = at > lo ? at : lo;
                info.size = (at + length < hi ? at + length : hi) - info.base;
                info.perm = protection.protection & 7;
            } else if (hid_mapping && regs[2] >= hid_mapping && regs[2] - hid_mapping < 0x40000) {
                info.base = hid_mapping; info.size = 0x40000; info.state = 6; info.perm = 1;
            } else if (clock_mapping && regs[2] >= clock_mapping && regs[2] - clock_mapping < 0x1000) {
                info.base = clock_mapping; info.size = 0x1000; info.state = 6; info.perm = 1;
            } else if (address_space && regs[2] >= (uint64_t)address_space + PROBE_ALIAS_OFF &&
                       regs[2] - ((uint64_t)address_space + PROBE_ALIAS_OFF) < PROBE_ALIAS_SIZE) {
                size_t page = (regs[2] - ((uint64_t)address_space + PROBE_ALIAS_OFF)) >> 12;
                size_t lo = page, hi = page + 1; uint8_t used = physical_pages[page];
                while (lo && physical_pages[lo - 1] == used) lo--;
                while (hi < (PROBE_ALIAS_SIZE >> 12) && physical_pages[hi] == used) hi++;
                info.base = (uint64_t)address_space + PROBE_ALIAS_OFF + (lo << 12);
                info.size = (hi - lo) << 12; info.state = used == 2 ? 6 : used ? 5 : 0; info.perm = used == 3 ? 0 : used == 2 ? 1 : used ? 3 : 0; info.attr = used == 3 ? 1 : 0;
            } else if (heap_size && regs[2] >= (uint64_t)address_space + PROBE_HEAP_OFF &&
                       regs[2] - ((uint64_t)address_space + PROBE_HEAP_OFF) < heap_size) {
                info.base = (uint64_t)address_space + PROBE_HEAP_OFF; info.size = heap_size; info.state = 5; info.perm = 3;
            } else if (regs[2] < bottom || regs[2] >= top) {
                // 只暴露客体拥有的映射；空洞边界来自相邻模块/栈，后续映射仍须限于预留区。
                // Expose only guest-owned mappings; holes are bounded by neighboring modules/stack, with future mappings restricted to the reservation.
                uint64_t lo = 0, hi = 1ull << 48;
                for (int k = 0; k <= g_nmod + 4 + (int)nstack; k++) {
                    uint64_t a, z;
                    if (k < g_nmod) { a = (uint64_t)g_mod[k].base - g_mod[k].image_off; z = (uint64_t)g_mod[k].base + g_mod[k].size; }
                    else if (k == g_nmod) { a = bottom; z = top; }
                    else if (k == g_nmod + 2) { if (!address_space) continue; a = (uint64_t)address_space + PROBE_ALIAS_OFF; z = a + PROBE_ALIAS_SIZE; }
                    else if (k == g_nmod + 3) { if (!clock_mapping) continue; a = clock_mapping; z = a + 0x1000; }
                    else if (k == g_nmod + 4) { if (!hid_mapping) continue; a = hid_mapping; z = a + 0x40000; }
                    else if (k > g_nmod + 4) { unsigned j = k - g_nmod - 5; a = stack_maps[j].dst; z = a + stack_maps[j].size; }
                    else { if (!heap_size) continue; a = (uint64_t)address_space + PROBE_HEAP_OFF; z = a + heap_size; }
                    if (z <= regs[2] && z > lo) lo = z;
                    if (a > regs[2] && a < hi) hi = a;
                }
                info.base = lo; info.size = hi - lo; info.state = 0; info.perm = 0;
            }

            for (unsigned k = 0; k < nstack; k++) if (regs[2] >= stack_maps[k].dst && regs[2] - stack_maps[k].dst < stack_maps[k].size) {
                info.base = stack_maps[k].dst; info.size = stack_maps[k].size; info.state = 0xb; info.attr = 0; info.perm = 3;
            }
            // 逻辑锁定也须切分 QueryMemory 的邻接区间，不能把仍可写的子页报成整段可写。
            // Logical locks must split neighboring QueryMemory ranges, including shared host subpages.
            for (unsigned k = 0; k < nstack; k++) {
                uint64_t a = stack_maps[k].src, z = a + stack_maps[k].size, end = info.base + info.size;
                if (regs[2] >= a && regs[2] < z) {
                    info.base = a; info.size = z - a; info.attr = 1; info.perm = 0;
                } else if (z <= regs[2] && z > info.base) { info.base = z; info.size = end - z; }
                else if (a > regs[2] && a < end) info.size = a - info.base;
            }
            memcpy((void *)regs[0], &info, sizeof info);
            regs[0] = 0; regs[1] = 0;
            LOG("PROBE QueryMemory base=%#llx size=%#llx state=%u permissions=%u OK", info.base, info.size, info.state, info.perm);
            return 0;
        }
        if (!strcmp(gates[i].kind, "svc") && gates[i].arg == 0x29 &&
                regs[2] == 0xffff8001 && regs[3] == 0) {
            PROBE_LOCK();
            uint64_t value;
            memory_init();
            switch (regs[1]) {
                case 0: value = 7; break;
                case 6: case 21: value = 4ull << 30; break;
                case 7: case 22: value = image_bytes() + heap_size; break;
                // 没有调试器/用户异常上下文；物理页状态由宿主独立维护。
                // No debugger or user-exception context; physical-page state is maintained by the host.
                case 8: case 20: value = 0; break;
                case 16: value = 0x2000000; break;
                case 17: value = PROBE_ALIAS_SIZE >> 12; break;   // 实际页状态表字节数 | Actual page-state table bytes
                case 23: value = 1; break;
                case 2: value = (uint64_t)address_space + PROBE_ALIAS_OFF; break;
                case 3: value = PROBE_ALIAS_SIZE; break;
                case 4: value = (uint64_t)address_space + PROBE_HEAP_OFF; break;
                case 5: value = PROBE_SHARED_OFF - PROBE_HEAP_OFF; break;
                case 12: value = (uint64_t)address_space; break;
                case 13: value = PROBE_AS_SIZE; break;
                case 14: value = (uint64_t)address_space + PROBE_STACK_OFF; break;
                case 15: value = 0x10000000; break;
                default: goto unsupported;
            }
            LOG("PROBE GetInfo region=%llu value=%#llx OK", regs[1], value);
            regs[0] = 0; regs[1] = value; return 0;
        }
unsupported:;
        if (!strcmp(gates[i].kind, "svc") && gates[i].arg == 0x26) {
            uint32_t error = 0; vm_size_t read = 0;
            if (regs[2] == 4 && vm_read_overwrite(mach_task_self(), regs[1], 4, (vm_address_t)&error, &read) == KERN_SUCCESS)
                LOG("PROBE SDK Break result=%#x", error);
            uint64_t fp = regs[29];
            for (int k = 0; k < 12; k++) {
                uint64_t frame[2];
                if (vm_read_overwrite(mach_task_self(), fp, 16, (vm_address_t)frame, &read) != KERN_SUCCESS || read != 16) break;
                module_t *owner = module_of((void *)frame[1]);
                if (owner) LOG("PROBE frame %s+%#llx", owner->name, frame[1] - (uint64_t)owner->base);
                if (frame[0] <= fp) break;
                fp = frame[0];
            }
        }
        if (!strcmp(gates[i].kind, "svc") && gates[i].arg == 0x21) {
            uint32_t *ipc = pthread_getspecific(g_keytlr);
            if (ipc) LOG("PROBE rejected IPC %08x %08x %08x %08x %08x %08x %08x %08x", ipc[0], ipc[1], ipc[2], ipc[3], ipc[4], ipc[5], ipc[6], ipc[7]);
        }
        module_t *m = module_of((void *)ret);
        LOG("PROBE STOP gate %s+%#llx %s %#x x0=%#llx x1=%#llx x2=%#llx x3=%#llx",
            m ? m->name : "?", gates[i].orig, gates[i].kind, gates[i].arg,
            regs[0], regs[1], regs[2], regs[3]);
        exit(3);
    }
    die("probe unknown gate %#llx", ret);
}
// 信号现场只用 write 输出十六进制，避免崩溃时再次进入 stdio/堆锁。
// Emit hexadecimal signal context with write only, avoiding stdio/heap locks during a crash.
static void crash_signal(int signal, siginfo_t *info, void *context) {
    ucontext_t *uc = context;
    uint64_t values[] = {(uint64_t)signal, (uint64_t)info->si_addr, uc->uc_mcontext->__ss.__pc,
        uc->uc_mcontext->__ss.__sp, uc->uc_mcontext->__ss.__lr, uc->uc_mcontext->__ss.__fp,
        uc->uc_mcontext->__ss.__x[0], uc->uc_mcontext->__ss.__x[1], uc->uc_mcontext->__ss.__x[2],
        uc->uc_mcontext->__ss.__x[8], uc->uc_mcontext->__ss.__x[19], uc->uc_mcontext->__ss.__x[20]};
    const char *labels[] = {"signal", "fault", "pc", "sp", "lr", "fp", "x0", "x1", "x2", "x8", "x19", "x20"};
    for (unsigned i = 0; i < sizeof values / sizeof *values; i++) {
        char line[64]; unsigned n = 0;
        const char *prefix = "PROBE CRASH "; while (*prefix) line[n++] = *prefix++;
        for (const char *p = labels[i]; *p; p++) line[n++] = *p;
        line[n++] = '='; line[n++] = '0'; line[n++] = 'x';
        for (int shift = 60; shift >= 0; shift -= 4) line[n++] = "0123456789abcdef"[(values[i] >> shift) & 15];
        line[n++] = '\n'; (void)write(2, line, n);
    }
    _exit(128 + signal);
}
static void runtime_ready(void) { LOG("PROBE callback runtime-ready"); }
static void constructors_ready(void) {
    // 保留 SDK 启动顺序，构造期间遇到未知服务仍由门分派中止。
    // Preserve SDK startup order; unknown services still stop at the gate during construction.
    LOG("PROBE entering module constructors");
    // 输入为 main、subsdk、sdk；依赖先构造，避免 SDK 构造清空游戏刚初始化的服务。
    run_module_inits(1);
    LOG("PROBE module constructors finished");
}
int main(int argc, char **argv) {
    const char *start = NULL;
    int first = 2;
    if (argc > 3 && !strcmp(argv[2], "--start")) { start = argv[3]; first = 4; }
    if (argc <= first || argc - first > MAX_MOD) {
        fprintf(stderr, "usage: module_probe DIR [--start SYMBOL] MODULE...\n");
        return 2;
    }
    // 探针限时且禁用 core 文件，避免挂起或落盘整份游戏内存。
    // Bound execution and disable core dumps to avoid hangs or writing game memory to disk.
    struct rlimit core = {0, 0};
    if (setrlimit(RLIMIT_CORE, &core)) return 2;
#ifdef DM_PROBE_METAL
    void crash_install(void); crash_install();
#else
    struct sigaction action = {.sa_sigaction = crash_signal, .sa_flags = SA_SIGINFO};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGSEGV, &action, NULL) || sigaction(SIGBUS, &action, NULL) || sigaction(SIGILL, &action, NULL)) return 2;
#endif
    // 长视频启动需更长观察窗口；仍有上限，避免失控探针长期占用远端。
    // Long startup movies need a larger, still bounded observation window.
    if (!getenv("DM_PROBE_INTERACTIVE")) alarm(240);
    load_modules(argv[1], (const char **)(argv + first), argc - first);
    LOG("PROBE LOAD OK modules=%d unresolved-strong=%u gates=%u", g_nmod, nmissing, ngates);
    if (nmissing) return 1;
    if (!start) return 0;
    void (*entry)(uint64_t, uint64_t, void (*)(void), void (*)(void)) = guest_sym(start);
    if (!entry) { LOG("PROBE missing start symbol %s", start); return 1; }
    // 四参数 SDK 启动传入真实主线程句柄；未知服务仍明确停止。
    // Pass the real main-thread handle to the four-argument SDK entry; unknown services still stop explicitly.
    guest_thread_init();
    uint32_t main_handle = svc_thread_main_init();
    LOG("PROBE SDK START %s", start);
    entry(main_handle, 0, runtime_ready, constructors_ready);
    LOG("PROBE start returned unexpectedly");
    return 1;
}
