// 穿过真实门分派检查探针服务 ABI、进程熵稳定性及未支持请求拒绝，使用合成参数。
// Exercise the real probe gate dispatch with synthetic arguments to check ABI, stable process entropy and unsupported-request rejection.
#define DM_PROBE_CAPTURE_TEST
#define main probe_main
#include "module_probe.c"
#undef main
#include <assert.h>
#include <sys/wait.h>
#include <mach/mach.h>

static int constructor_order, constructor_service;
static void init_sdk_fixture(void) { constructor_order = constructor_order * 10 + 3; constructor_service = 0; }
static void init_library_fixture(void) { constructor_order = constructor_order * 10 + 2; }
static void init_game_fixture(void) { constructor_order = constructor_order * 10 + 1; constructor_service = 1; }

int main(void) {
    // SDK 全局构造不能在游戏已初始化服务后清空指针；穿过真实构造回调复现这个顺序错误。
    void (*initializers[])(void) = {init_game_fixture, init_library_fixture, init_sdk_fixture};
    const char *module_names[] = {"main", "subsdk0", "sdk"};
    assert(!g_nmod);
    for (int i = 0; i < 3; i++) {
        strlcpy(g_mod[i].name, module_names[i], sizeof g_mod[i].name);
        g_mod[i].base = (uint8_t *)((uintptr_t)initializers[i] - 1); g_mod[i].init = 1;
    }
    g_nmod = 3; constructors_ready();
    assert(constructor_order == 321 && constructor_service == 1);
    constructor_order = 0; run_module_inits(0);
    assert(constructor_order == 123 && constructor_service == 0);
    constructor_order = 0; g_mod[1].init = 0; constructors_ready();
    assert(constructor_order == 31 && constructor_service == 1);
    g_nmod = 0; memset(g_mod, 0, sizeof g_mod);
    assert(probe_use_native_vp9("video/x-vnd.on2.vp9", 0, NULL));
    assert(!probe_use_native_vp9("video/x-vnd.on2.vp9", 1, NULL));
    assert(!probe_use_native_vp9("video/x-vnd.on2.vp9", 0, "explicit-decoder"));
    assert(!probe_use_native_vp9("audio/vorbis", 0, NULL));
    // 使用合成着色器验证捕获长度、内容与重复调用，不包含游戏字节。
    // Synthetic shaders verify capture bounds, content and repeat calls without game bytes.
    char capture_dir[] = "/tmp/dm-shader-capture-XXXXXX"; assert(mkdtemp(capture_dir));
    assert(!setenv("DM_PROBE_SHADER_DUMP", capture_dir, 1));
    uint8_t control[0x880] = {0}, code[256]; memset(code, 0x5a, sizeof code);
    uint32_t signature = 0x98761234, shader_size = sizeof code;
    memcpy(control, &signature, 4); memcpy(control + 0x704, &shader_size, 4);
    probe_shader_dump(control, (uint64_t)code); probe_shader_dump(control, (uint64_t)code);
    uint8_t hash[32]; CC_SHA256(control, sizeof control, hash); char key[17], path[4096];
    for (int k = 0; k < 8; k++) snprintf(key + 2 * k, 3, "%02x", hash[k]);
    for (int part = 0; part < 2; part++) {
        snprintf(path, sizeof path, "%s/%s.%s", capture_dir, key, part ? "code" : "control");
        int fd = open(path, O_RDONLY); assert(fd >= 0); uint8_t contents[0x880];
        size_t bytes = part ? sizeof code : sizeof control; assert(read(fd, contents, sizeof contents) == bytes);
        assert(!memcmp(contents, part ? code : control, bytes)); close(fd); unlink(path);
    }
    pid_t invalid_shader = fork(); assert(invalid_shader >= 0);
    if (!invalid_shader) { shader_size = 0x100001; memcpy(control + 0x704, &shader_size, 4); probe_shader_dump(control, (uint64_t)code); _exit(0); }
    int shader_status; assert(waitpid(invalid_shader, &shader_status, 0) == invalid_shader);
    assert(!WIFEXITED(shader_status) || WEXITSTATUS(shader_status) != 0);
    unsetenv("DM_PROBE_SHADER_DUMP"); assert(!rmdir(capture_dir));
    uint32_t handle = svc_thread_main_init();
    gate_register(0x3000, "svc", 0xc, 0);
    uint64_t priority[31] = {0, handle};
    gate_dispatch(0x3000, priority);
    assert(priority[0] == 0 && priority[1] == 44);
    priority[1] = 0;
    gate_dispatch(0x3000, priority);
    assert(priority[0] != 0);
    gate_register(0x1000, "svc", 0x29, 0);
    gate_register(0x2000, "svc", 6, 0);
    uint64_t values[4];
    for (unsigned round = 0; round < 2; round++) for (unsigned index = 0; index < 4; index++) {
        uint64_t regs[31];
        for (unsigned i = 0; i < 31; i++) regs[i] = 0x10000 + i;
        regs[0] = 1; regs[1] = 11; regs[2] = 0; regs[3] = index;
        gate_dispatch(0x1000, regs);
        assert(regs[0] == 0 && regs[2] == 0 && regs[3] == index);
        for (unsigned i = 4; i < 31; i++) assert(regs[i] == 0x10000 + i);
        if (!round) values[index] = regs[1];
        else assert(values[index] == regs[1]);
    }
    // 不支持的参数不能默认为成功；每个子进程必须走明确的边界退出。
    // Unsupported arguments must not default to success; each child must take the explicit boundary exit.
    for (int kind = 0; kind < 5; kind++) {
        pid_t pid = fork(); assert(pid >= 0);
        if (!pid) {
            uint64_t regs[31] = {0, kind == 0 ? 12 : 11, kind == 1 ? 1 : 0, kind == 2 ? 4 : 0};
            if (kind >= 3) {
                regs[0] = kind == 3 ? 1 : (uint64_t)regs;
                regs[2] = kind == 3 ? (uint64_t)regs : (1ull << 48);
            }
            gate_dispatch(kind < 3 ? 0x1000 : 0x2000, regs); _exit(99);
        }
        int status; assert(waitpid(pid, &status, 0) == pid);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 3);
    }
    uint64_t info[6] = {0}; info[5] = 0x12345678;
    uint64_t query[31] = {(uint64_t)info, 1, (uint64_t)&query, 0x5678};
    gate_dispatch(0x2000, query);
    assert(query[0] == 0 && query[1] == 0 && query[3] == 0x5678);
    assert(info[0] == (uint64_t)pthread_get_stackaddr_np(pthread_self()) - pthread_get_stacksize_np(pthread_self()));
    assert(info[1] == pthread_get_stacksize_np(pthread_self()));
    assert(((uint32_t *)info)[4] == 0xb && ((uint32_t *)info)[6] == 3);
    assert(info[5] == 0x12345678);
    uint64_t region[16] = {0};
    const unsigned ids[] = {2, 3, 4, 5, 12, 13, 14, 15};
    for (unsigned i = 0; i < sizeof ids / sizeof *ids; i++) {
        uint64_t regs[31] = {1, ids[i], 0xffff8001, 0};
        gate_dispatch(0x1000, regs);
        assert(regs[0] == 0 && regs[2] == 0xffff8001 && regs[3] == 0);
        region[ids[i]] = regs[1];
    }
    assert(region[12] == (uint64_t)address_space && region[13] == PROBE_AS_SIZE);
#if DM_PROBE_COMPACT_AS
    // 共享映射只能选中宿主接受的区间；栈与非完整区间不能被误改。
    assert(probe_shared_begin(region[12], region[12] + region[13]) == region[12] + PROBE_SHARED_OFF);
    assert(probe_shared_begin(region[14], region[14] + region[15]) == region[14]);
    assert(probe_shared_begin(region[12], region[12] + region[13] - 0x1000) == region[12]);
#endif
    // 跨过 39 位边界的客体地址须恢复；不属于客体区的地址保持原值。
    uint8_t *saved_space = address_space;
    address_space = (uint8_t *)((1ull << 39) + (64ull << 30));
    uint64_t target = (uint64_t)address_space + PROBE_STACK_OFF + 0x1000;
    uint64_t wire = target & ((1ull << 39) - 1);
    uint32_t buffer_desc[3] = {0x50, (uint32_t)wire, (uint32_t)(((wire >> 32) & 15) << 28) | (uint32_t)(((wire >> 36) & 7) << 2)};
    uint64_t decoded_address, bytes;
    assert(ipc_buffer(buffer_desc, &decoded_address, &bytes) && decoded_address == target && bytes == 0x50);
    wire = 0x1000; assert(probe_ipc_address(&wire, 0x50) && wire == 0x1000);
    wire = ((uint64_t)address_space + PROBE_AS_SIZE - 8) & ((1ull << 39) - 1);
    assert(!probe_ipc_address(&wire, 9));
    address_space = saved_space;
    assert(region[2] >= region[12] && region[2] + region[3] <= region[14]);
    assert(region[14] + region[15] <= region[4]);
    assert(region[4] + region[5] == region[12] + PROBE_SHARED_OFF);
    assert(region[4] + region[5] < region[12] + region[13]);
    // macOS 可把一次大预留拆成多个 VM 条目，逐段验证连续覆盖及不可访问权限。
    // macOS may split a large reservation into VM entries; verify contiguous coverage and protection entry by entry.
    vm_address_t cursor = (vm_address_t)address_space;
    while (cursor < (vm_address_t)address_space + PROBE_AS_SIZE) {
        vm_address_t address = cursor;
        vm_size_t length;
        vm_region_basic_info_data_64_t protection;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object;
        assert(vm_region_64(mach_task_self(), &address, &length, VM_REGION_BASIC_INFO_64,
                           (vm_region_info_t)&protection, &count, &object) == KERN_SUCCESS);
        assert(address <= cursor && address + length > cursor);
        assert(protection.protection == VM_PROT_NONE);
        if (object) mach_port_deallocate(mach_task_self(), object);
        cursor = address + length;
    }
    // 4 KiB 客体页共享 16 KiB 宿主页：撤销一个不能破坏相邻页。
    // Guest 4 KiB pages share host 16 KiB pages: unmapping one must preserve its neighbor.
    gate_register(0x4000, "svc", 0x2c, 0); gate_register(0x5000, "svc", 0x2d, 0);
    uint64_t alias = region[2];
    uint64_t map[31] = {alias, 0x2000}; gate_dispatch(0x4000, map); assert(!map[0]);
    *(uint64_t *)(alias + 0x1000) = 0x9876;
    uint64_t unmap[31] = {alias, 0x1000}; gate_dispatch(0x5000, unmap); assert(!unmap[0]);
    assert(*(uint64_t *)(alias + 0x1000) == 0x9876);
    query[0] = (uint64_t)info; query[2] = alias; gate_dispatch(0x2000, query);
    assert(info[0] == alias && info[1] == 0x1000 && ((uint32_t *)info)[4] == 0);
    query[0] = (uint64_t)info; query[2] = alias + 0x1000; gate_dispatch(0x2000, query);
    assert(info[0] == alias + 0x1000 && info[1] == 0x1000 && ((uint32_t *)info)[4] == 5);
    // 栈移动往返保留数据，源区在移动期间不可访问。
    // Stack moves round-trip data while making the source inaccessible during the move.
    gate_register(0x6000, "svc", 4, 0); gate_register(0x7000, "svc", 5, 0);
    map[0] = alias + 0x4000; map[1] = 0x4000; gate_dispatch(0x4000, map);
    *(uint64_t *)(alias + 0x4000) = 42;
    uint64_t move[31] = {region[14] + 0x1000, alias + 0x4000, 0x4000};
    gate_dispatch(0x6000, move); assert(!move[0]);
    assert(*(uint64_t *)(region[14] + 0x1000) == 42);
    *(uint64_t *)(region[14] + 0x1000) = 43;
    move[0] = region[14] + 0x1000; gate_dispatch(0x7000, move); assert(!move[0]);
    assert(*(uint64_t *)(alias + 0x4000) == 43);
    // 合成模块覆盖整宿主页和共享子页的栈移动，并拒绝只读、键页、越界和重复源。
    // A synthetic module covers full-page/subpage moves and rejects read-only, key, out-of-range and duplicate sources.
    uint8_t *module = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(module != MAP_FAILED && !g_nmod);
    g_mod[0] = (module_t){.base = module, .size = 0x10000, .data_off = 0x4000, .key_page_off = 0xc000, .key_page_size = 0x4000};
    g_nmod = 1;
    for (unsigned subpage = 0; subpage < 2; subpage++) {
        uint64_t src = (uint64_t)module + 0x4000 + subpage * 0x1000, bytes = subpage ? 0x1000 : 0x4000;
        *(uint64_t *)src = 123;
        uint64_t mapped_stack[31] = {region[14] + 0x1000, src, bytes};
        gate_dispatch(0x6000, mapped_stack); assert(!mapped_stack[0]);
        query[0] = (uint64_t)info; query[2] = src; gate_dispatch(0x2000, query);
        assert(info[0] == src && info[1] == bytes && ((uint32_t *)info)[5] == 1 && ((uint32_t *)info)[6] == 0);
        if (subpage) {
            query[0] = (uint64_t)info; query[2] = src - 0x1000; gate_dispatch(0x2000, query);
            assert(info[0] + info[1] == src && ((uint32_t *)info)[6] == 3);
        }
        pid_t duplicate = fork(); assert(duplicate >= 0);
        if (!duplicate) { uint64_t again[31] = {region[14] + 0x10000, src, bytes}; gate_dispatch(0x6000, again); _exit(99); }
        int status; assert(waitpid(duplicate, &status, 0) == duplicate && WIFEXITED(status) && WEXITSTATUS(status) == 3);
        assert(*(uint64_t *)(region[14] + 0x1000) == 123);
        *(uint64_t *)(region[14] + 0x1000) = 456;
        mapped_stack[0] = region[14] + 0x1000; gate_dispatch(0x7000, mapped_stack);
        assert(!mapped_stack[0] && *(uint64_t *)src == 456);
    }
    for (unsigned invalid = 0; invalid < 4; invalid++) {
        pid_t child = fork(); assert(child >= 0);
        if (!child) {
            uint64_t src = (uint64_t)module + (invalid == 0 ? 0 : invalid == 1 ? 0xc000 : invalid == 2 ? 0xb000 : 0x4000);
            if (invalid == 3) assert(!mprotect(module + 0x4000, 0x4000, PROT_READ));
            uint64_t bad[31] = {region[14] + 0x1000, src, invalid == 2 ? 0x2000 : 0x1000};
            gate_dispatch(0x6000, bad); _exit(99);
        }
        int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 3);
    }
    g_nmod = 0; memset(g_mod, 0, sizeof g_mod[0]); assert(!munmap(module, 0x10000));
    // 域会话克隆共享对象身份，不能误发为新的根对象。
    // Cloned domain sessions share object identity rather than introducing a new root object.
    assert(!pthread_key_create(&g_keytlr, NULL)); uint32_t ipc[64] = {0}; pthread_setspecific(g_keytlr, ipc);
    gate_register(0x8000, "svc", 0x21, 0); sm_connected = 1;
    uint32_t manager = new_session("audren:u");
    ipc[0] = 6; ipc[1] = 12; ipc[4] = 0x49434653; ipc[6] = 4; ipc[8] = 0x37564552;
    uint64_t audio_send[31] = {manager}; gate_dispatch(0x8000, audio_send);
    assert(!audio_send[0] && ipc[2] == 0x20); uint32_t device = ipc[3];
    for (int round = 0; round < 2; round++) {
        memset(ipc, 0, sizeof ipc); ipc[0] = 6; ipc[1] = 8; ipc[4] = 0x49434653; ipc[6] = 12;
        audio_send[0] = device; gate_dispatch(0x8000, audio_send);
        assert(!audio_send[0] && ipc[2] == 2 && ipc[3] == event_read[device - 0x8000]);
    }
    memset(ipc, 0, sizeof ipc); ipc[0] = 6; ipc[1] = 8; ipc[4] = 0x49434653; ipc[6] = 5;
    audio_send[0] = device; gate_dispatch(0x8000, audio_send); assert(!audio_send[0] && ipc[8] == 2);
    memset(ipc, 0, sizeof ipc);
    uint32_t session = new_session("fsp-srv");
    ipc[0] = 7; ipc[1] = 8; ipc[4] = 0x49434653;
    uint64_t send[31] = {session}; gate_dispatch(0x8000, send);
    assert(ipc[4] == 0x4f434653 && ipc[8] == session - 0x8000 + 1);
    memset(ipc, 0, sizeof ipc); ipc[0] = 7; ipc[1] = 8; ipc[4] = 0x49434653; ipc[6] = 2;
    send[0] = session; gate_dispatch(0x8000, send);
    uint32_t clone = ipc[3]; assert(clone != session && session_owner[clone - 0x8000] == session - 0x8000);
    initialized[session - 0x8000] = 1;
    memset(ipc, 0, sizeof ipc); ipc[0] = 6; ipc[1] = 12; ipc[4] = 0x100001;
    ipc[5] = session - 0x8000 + 1; ipc[8] = 0x49434653; ipc[10] = 1005;
    send[0] = clone; gate_dispatch(0x8000, send);
    assert(!send[0] && ipc[8] == 0x4f434653 && !ipc[10] && !ipc[12]);
    // 域对象关闭后仍可关闭承载会话；两个事件句柄必须使 CMIF 正确跨过 16 字节边界。
    // A domain object may close before its transport session; two event handles must realign CMIF correctly.
    memset(ipc, 0, sizeof ipc); ipc[0] = 6; ipc[1] = 8; ipc[4] = 2; ipc[5] = session - 0x8000 + 1;
    send[0] = session; gate_dispatch(0x8000, send); assert(!send[0]);
    memset(ipc, 0, sizeof ipc); ipc[0] = 2; send[0] = session; gate_dispatch(0x8000, send); assert(send[0] == 0xf601);
    uint32_t request = new_session("netreq");
    memset(ipc, 0, sizeof ipc); ipc[0] = 6; ipc[1] = 8; ipc[4] = 0x49434653; ipc[6] = 2;
    send[0] = request; gate_dispatch(0x8000, send);
    assert(!send[0] && ipc[2] == 4 && ipc[3] != ipc[4] && ipc[8] == 0x4f434653);
    // 合成只读存储验证非零偏移读取，不引入任何游戏字节。
    // Synthetic read-only storage checks nonzero-offset reads without introducing game bytes.
    char storage_path[] = "/tmp/dm-probe-storage-XXXXXX";
    romfs_fd = mkstemp(storage_path); assert(romfs_fd >= 0); unlink(storage_path);
    unsigned char source[32], destination[16]; for (unsigned k = 0; k < sizeof source; k++) source[k] = k;
    assert(write(romfs_fd, source, sizeof source) == sizeof source); romfs_bytes = sizeof source;
    uint32_t storage = new_session("romstore"); uint64_t address = (uint64_t)destination;
    memset(ipc, 0, sizeof ipc); ipc[0] = 6 | (1 << 24); ipc[1] = 14;
    ipc[2] = sizeof destination; ipc[3] = address; ipc[4] = ((address >> 32) & 15) << 28 | ((address >> 36) & 7) << 2;
    ipc[8] = 0x49434653; ipc[12] = 7; ipc[14] = sizeof destination;
    send[0] = storage; gate_dispatch(0x8000, send);
    assert(!send[0] && !memcmp(destination, source + 7, sizeof destination)); close(romfs_fd); romfs_fd = -1;
    // 验证映射回收后清零，不能泄漏旧页内容。
    // Recommitting a released page must clear its old contents.
    map[0] = alias; map[1] = 0x1000; gate_dispatch(0x4000, map); assert(!map[0]);
    assert(*(uint64_t *)alias == 0 && *(uint64_t *)(alias + 0x1000) == 0x9876);
    // 发送方关闭 transfer 句柄后，服务已复制的引用仍须可写。
    // A service's copied transfer reference survives closing the sender handle.
    uint8_t copyright_pixels[16] = {0};
    transfers[ntransfer] = (typeof(transfers[0])){(uint64_t)copyright_pixels, sizeof copyright_pixels, 0, 1};
    uint32_t transfer = 0x9000 + ntransfer++, application = new_session("appfunc");
    memset(ipc, 0, sizeof ipc); ipc[0] = 6; ipc[1] = 0x8000000c; ipc[2] = 2; ipc[3] = transfer;
    ipc[4] = 0x49434653; ipc[6] = 100; ipc[8] = 2; ipc[9] = 2; ipc[10] = sizeof copyright_pixels;
    send[0] = application; gate_dispatch(0x8000, send); assert(!send[0]);
    gate_register(0xa000, "svc", 0x16, 0); uint64_t close_transfer[31] = {transfer};
    gate_dispatch(0xa000, close_transfer); assert(!close_transfer[0]);
    uint8_t stamp[4] = {1, 2, 3, 4}; uint64_t stamp_address = (uint64_t)stamp;
    memset(ipc, 0, sizeof ipc); ipc[0] = 6 | (1 << 20); ipc[1] = 14;
    ipc[2] = 4; ipc[3] = stamp_address; ipc[4] = ((stamp_address >> 32) & 15) << 28 | ((stamp_address >> 36) & 7) << 2;
    ipc[8] = 0x49434653; ipc[10] = 101; ipc[12] = 1; ipc[13] = 1; ipc[14] = 1; ipc[15] = 1; ipc[16] = 1;
    send[0] = application; gate_dispatch(0x8000, send);
    assert(!send[0] && !memcmp(copyright_pixels + 12, stamp, 4));
    // 共享时钟按公开双缓冲布局解码，结果必须等于宿主墙钟。
    // Decode the public double-buffered clock layout and compare with host wall time.
    clock_handle = new_session("clockmem"); gate_register(0x9000, "svc", 0x13, 0);
    uint64_t clock_addr = (uint64_t)address_space + PROBE_SHARED_OFF + 0x10000;
    uint64_t clock_regs[31] = {clock_handle, clock_addr, 0x1000, 1};
    gate_dispatch(0x9000, clock_regs); assert(!clock_regs[0]);
    int64_t base, offset; memcpy(&base, (void *)(clock_addr + 8), 8); memcpy(&offset, (void *)(clock_addr + 0x40), 8);
    int64_t decoded = offset + (base + (int64_t)now_ns()) / 1000000000;
    assert(llabs(decoded - (int64_t)time(NULL)) <= 1);
    assert(!memcmp((void *)(clock_addr + 16), (void *)(clock_addr + 0x50), 16));
    assert(!memcmp((void *)(clock_addr + 8), (void *)(clock_addr + 32), 24));
    assert(!memcmp((void *)(clock_addr + 0x40), (void *)(clock_addr + 0x60), 32));
    // HID 空输入状态必须是只读共享页，且不能覆盖时钟或线程栈。
    // Empty HID state must be read-only shared memory without overlapping clocks or stacks.
    hid_handle = new_session("hidmem");
    uint64_t hid_addr = (uint64_t)address_space + PROBE_SHARED_OFF + 0x40000;
    uint64_t wrong_region[31] = {hid_handle, alias + 0x10000, 0x40000, 1};
    gate_dispatch(0x9000, wrong_region); assert(wrong_region[0] == 0xdc01 && !hid_mapping);
    uint64_t hid_regs[31] = {hid_handle, hid_addr, 0x40000, 1};
    gate_dispatch(0x9000, hid_regs); assert(!hid_regs[0] && hid_mapping == hid_addr);
    for (size_t k = 0; k < 0x40000; k++) assert(!((uint8_t *)hid_addr)[k]);
    query[0] = (uint64_t)info; query[2] = hid_addr; gate_dispatch(0x2000, query);
    assert(info[0] == hid_addr && info[1] == 0x40000 && ((uint32_t *)info)[6] == 1);
    uint32_t hid_service = new_session("hid");
    memset(ipc, 0, sizeof ipc); ipc[0] = 6; ipc[1] = 12; ipc[4] = 0x49434653; ipc[6] = 109;
    send[0] = hid_service; gate_dispatch(0x8000, send); assert(!send[0] && hid_active);
    uint64_t *ring = (void *)(hid_addr + 0x9a00 + 0x28);
    assert(ring[1] == 16 && ring[2] == 1 && ring[3] == 15);
    assert(ring[4 + 6] == 17 && ring[4 + 6 + 1] == 17 && !ring[4 + 6 + 2]);
    uint32_t pad_ids[2] = {0, 32}; uint64_t pad_address = (uint64_t)pad_ids;
    memset(ipc, 0, sizeof ipc); ipc[0] = 6 | (1 << 16); ipc[1] = 8;
    ipc[2] = (sizeof pad_ids << 16) | ((pad_address & 0x0f00000000ull) >> 20) | ((pad_address & 0x7000000000ull) >> 30);
    ipc[3] = pad_address; ipc[4] = 0x49434653; ipc[6] = 102;
    send[0] = hid_service; gate_dispatch(0x8000, send); assert(!send[0] && hid_supported == 0x101);
    memset(ipc, 0, sizeof ipc); ipc[0] = 7; ipc[1] = 8; ipc[4] = 0x49434653; ipc[6] = 3;
    send[0] = hid_service; gate_dispatch(0x8000, send); assert(!send[0] && ipc[8] == 0x100);
    uint32_t vibration = new_session("viblist");
    for (int round = 0; round < 2; round++) {
        memset(ipc, 0, sizeof ipc); ipc[0] = 6; ipc[1] = 9; ipc[4] = 0x49434653; ipc[8] = 0x10003;
        send[0] = vibration; gate_dispatch(0x8000, send); assert(!send[0] && nvibration == 1);
    }
    memset(ipc, 0, sizeof ipc); ipc[0] = 6; ipc[1] = 9; ipc[4] = 0x49434653; ipc[6] = 200; ipc[8] = 0x10003;
    send[0] = hid_service; gate_dispatch(0x8000, send); assert(!send[0] && !ipc[8] && !ipc[9]);
    // 标题确认会发送震动值；无马达的虚拟手柄也必须消费完整请求而不是停止进程。
    // Title confirmation sends rumble; the motorless virtual pad must consume a complete request.
    memset(ipc, 0, sizeof ipc); ipc[0] = 6; ipc[1] = 0x80000010; ipc[2] = 1;
    ipc[8] = 0x49434653; ipc[10] = 201; ipc[12] = 0x10003;
    float rumble[4] = {0.25f, 160.f, 0.5f, 320.f}; memcpy(ipc + 13, rumble, sizeof rumble);
    uint64_t app_uid = 1; memcpy(ipc + 18, &app_uid, sizeof app_uid);
    send[0] = hid_service; gate_dispatch(0x8000, send); assert(!send[0] && !ipc[6]);
    // 截断请求或未激活的设备不能被无马达路径误接受。
    // Motorless handling must still reject truncated requests and inactive handles.
    for (int invalid = 0; invalid < 2; invalid++) {
        pid_t child = fork(); assert(child >= 0);
        if (!child) {
            memset(ipc, 0, sizeof ipc); ipc[0] = 6; ipc[1] = 0x80000010; ipc[2] = 1;
            ipc[8] = 0x49434653; ipc[10] = 201; ipc[12] = 0x10003;
            if (invalid) ipc[12] = 0xdeadbeef; else ipc[1] = 0x8000000e;
            send[0] = hid_service; gate_dispatch(0x8000, send); _exit(99);
        }
        int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 3);
    }
    // libnx HidNpadButton：十字键为 12～15 位，16～19 位是摇杆伪按钮。
    // Match the published D-pad ABI rather than left-stick pseudo-buttons.
    assert(probe_key_button(123) == (1ull << 12));
    assert(probe_key_button(126) == (1ull << 13));
    assert(probe_key_button(124) == (1ull << 14));
    assert(probe_key_button(125) == (1ull << 15));
    assert(probe_key_button(36) == 1 && probe_key_button(53) == 2 && !probe_key_button(255));
    probe_keyboard_ready = 1; probe_keyboard_buttons = probe_key_button(36);
    // 设置手柄类型后必须同步发布，不能等待被资源加载阻塞的采样线程。
    memset(ipc, 0, sizeof ipc); ipc[0] = 6; ipc[1] = 12;
    ipc[4] = 0x49434653; ipc[6] = 100; ipc[8] = 1;
    send[0] = hid_service; gate_dispatch(0x8000, send); assert(!send[0]);
    uint64_t *sample_entry = ring + 4 + ring[2] * 6;
    assert(*(uint32_t *)(hid_addr + 0x9a00) == 1 && sample_entry[2] == 1 && sample_entry[5] == 3);
    probe_keyboard_buttons = probe_key_button(13) | probe_key_button(2) | probe_key_button(38);
    hid_tick(); sample_entry = ring + 4 + ring[2] * 6;
    assert((int32_t)sample_entry[3] == 23170 && (int32_t)(sample_entry[3] >> 32) == 23170);
    assert((int32_t)sample_entry[4] == -32767 && !(sample_entry[4] >> 32));
    probe_keyboard_buttons = probe_key_button(0) | probe_key_button(2) | probe_key_button(13) | probe_key_button(1);
    hid_tick(); assert(!ring[4 + ring[2] * 6 + 3]);
    probe_keyboard_buttons = 0; hid_tick(); sample_entry = ring + 4 + ring[2] * 6;
    assert(!sample_entry[2] && !sample_entry[3] && !sample_entry[4]);
    probe_keyboard_buttons = probe_key_button(36) | probe_key_button(13);
    probe_keyboard_modal = 1; hid_tick(); sample_entry = ring + 4 + ring[2] * 6;
    assert(!sample_entry[2] && !sample_entry[3] && !sample_entry[4]);
    probe_keyboard_modal = 0;
    hid_tick(); sample_entry = ring + 4 + ring[2] * 6;
    assert((sample_entry[2] & 3) == 1 && (int32_t)(sample_entry[3] >> 32) == 32767);
    probe_keyboard_buttons = 0;
    probe_keyboard_ready = 0; hid_tick(); assert(!*(uint32_t *)(hid_addr + 0x9a00));
    hid_active = 0;
    pid_t overlapping = fork(); assert(overlapping >= 0);
    if (!overlapping) { hid_mapping = 0; hid_regs[0] = hid_handle; hid_regs[1] = clock_addr; gate_dispatch(0x9000, hid_regs); _exit(99); }
    int overlap_status; assert(waitpid(overlapping, &overlap_status, 0) == overlapping && WIFEXITED(overlap_status) && WEXITSTATUS(overlap_status) == 3);
    uint64_t resource[31] = {0, 17, 0xffff8001, 0}; gate_dispatch(0x1000, resource);
    assert(!resource[0] && resource[1] == PROBE_ALIAS_SIZE / 4096);
    // 日历字段顺序须匹配 Horizon ABI，不能把年份和偏移错位。
    // Calendar field order must match the Horizon ABI.
    // 本地身份查询必须精确匹配；存档准备只创建显式配置的独立目录。
    // Local identity checks must match exactly; save preparation creates only the configured directory.
    // 实际 IPC 分派必须改变退出保护，不能仅返回成功而允许保存期间关闭窗口。
    uint32_t self_control = new_session("self");
    assert(probe_can_close());
    for (unsigned command=1; command<=2; command++) {
        memset(ipc,0,sizeof ipc);ipc[0]=6;ipc[1]=8;ipc[4]=0x49434653;ipc[6]=command;
        send[0]=self_control;gate_dispatch(0x8000,send);
        assert(!send[0]&&!ipc[6]&&probe_can_close()==(command==2));
    }
    uint32_t account = new_session("acc:u0"); uint64_t local_uid[2] = {(uint64_t)getuid() + 1, 0x444d4c4f43414c};
    for (int known = 0; known < 2; known++) {
        memset(ipc, 0, sizeof ipc); ipc[0] = 6; ipc[1] = 12; ipc[4] = 0x49434653; ipc[6] = 1;
        if (known) memcpy(ipc + 8, local_uid, sizeof local_uid);
        send[0] = account; gate_dispatch(0x8000, send); assert(!send[0] && ipc[8] == known);
    }
    char save_parent[] = "/tmp/dqxi-save-test-XXXXXX"; assert(mkdtemp(save_parent));
    char save_path[256]; snprintf(save_path, sizeof save_path, "%s/save", save_parent); setenv("DM_PROBE_SAVE_DIR", save_path, 1);
    memset(ipc, 0, sizeof ipc); ipc[0] = 6; ipc[1] = 12; ipc[4] = 0x49434653; ipc[6] = 20;
    memcpy(ipc + 8, local_uid, sizeof local_uid); send[0] = application; gate_dispatch(0x8000, send);
    struct stat save_stat; assert(!send[0] && save_root_fd >= 0 && !fstat(save_root_fd, &save_stat) && S_ISDIR(save_stat.st_mode));
    uint32_t fs_session = new_session("fsp-srv"); initialized[fs_session - 0x8000] = 1;
    memset(ipc, 0, sizeof ipc); ipc[0] = 6; ipc[1] = 26; ipc[4] = 0x49434653; ipc[6] = 51;
    ipc[8] = 1; memcpy(ipc + 12, local_uid, sizeof local_uid); ipc[18] = 1;
    send[0] = fs_session; gate_dispatch(0x8000, send); assert(!send[0] && dm_save_workdir());
    memset(ipc, 0, sizeof ipc); ipc[0] = 6; ipc[1] = 26; ipc[4] = 0x49434653; ipc[6] = 51;
    ipc[8] = 1; ipc[10] = 0xea0a000; ipc[11] = 0x1002680; memcpy(ipc + 12, local_uid, sizeof local_uid); ipc[18] = 1;
    send[0] = fs_session; gate_dispatch(0x8000, send); assert(!send[0] && ipc[6] == ((1002u << 9) | 2u));
    memset(ipc, 0, sizeof ipc); ipc[0] = 7; ipc[1] = 8; ipc[4] = 0x49434653; ipc[6] = 3;
    send[0] = fs_session; gate_dispatch(0x8000, send); assert(!send[0] && ipc[8] >= 0x301);
    char guest_path[0x301] = "/test", checked_path[0x310]; uint64_t guest_address = (uint64_t)guest_path;
    uint32_t path_desc[] = {(sizeof guest_path << 16) | ((guest_address >> 20) & 0xf000) | ((guest_address >> 30) & 0x1c0), (uint32_t)guest_address};
    assert(probe_save_path(path_desc, checked_path) && !strcmp(checked_path, "save:/test"));
    strcpy(guest_path, "/missing/file"); assert(probe_save_path(path_desc, checked_path));
    int32_t missing_type; assert(h_fs_GetEntryType(&missing_type, checked_path) == RES_PATH_NOT_FOUND);
    strcpy(guest_path, "/missing/../outside"); assert(!probe_save_path(path_desc, checked_path));
    strcpy(guest_path, "/../outside"); assert(!probe_save_path(path_desc, checked_path));
    char link_path[1100]; snprintf(link_path, sizeof link_path, "%s/link", dm_save_workdir());
    assert(!symlink("/tmp", link_path)); strcpy(guest_path, "/link"); assert(!probe_save_path(path_desc, checked_path)); assert(!unlink(link_path));
    memset(guest_path, 'x', sizeof guest_path); assert(!probe_save_path(path_desc, checked_path));
    assert(!h_fs_CreateFile("save:/test", 3)); uint64_t save_file;
    // 穿过 IFile 的真实 IPC 打开、写入、读取、刷新和关闭，再核对提交后的磁盘内容。
    strcpy(guest_path,"/test");uint32_t savefs=new_session("savefs");
    memset(ipc,0,sizeof ipc);ipc[0]=6|(1<<16);ipc[1]=9;
    memcpy(ipc+2,path_desc,sizeof path_desc);ipc[4]=0x49434653;ipc[6]=8;ipc[8]=3;
    send[0]=savefs;gate_dispatch(0x8000,send);assert(!send[0]&&!ipc[6]);uint32_t file_session=ipc[3];
    assert(save_files[file_session-0x8000]);
    char file_bytes[3]="yes";uint64_t file_address=(uint64_t)file_bytes;
    for(int write=1;write>=0;write--) {
        if(!write)memset(file_bytes,0,sizeof file_bytes);
        memset(ipc,0,sizeof ipc);ipc[0]=6|(1<<(write?20:24));ipc[1]=18;
        ipc[2]=sizeof file_bytes;ipc[3]=file_address;ipc[4]=((file_address>>32)&15)<<28|((file_address>>36)&7)<<2;
        ipc[8]=0x49434653;ipc[10]=write;ipc[12]=write;ipc[16]=sizeof file_bytes;
        send[0]=file_session;gate_dispatch(0x8000,send);assert(!send[0]&&!ipc[6]);
        if(!write)assert(ipc[8]==3&&!memcmp(file_bytes,"yes",3));
    }
    for(unsigned command=2;command<=4;command++) {
        memset(ipc,0,sizeof ipc);ipc[0]=6;ipc[1]=10;ipc[4]=0x49434653;ipc[6]=command;ipc[8]=3;
        send[0]=file_session;gate_dispatch(0x8000,send);assert(!send[0]&&!ipc[6]);
        if(command==4)assert(ipc[8]==3);
    }
    is_domain[file_session-0x8000]=1;
    memset(ipc,0,sizeof ipc);ipc[0]=6;ipc[1]=8;ipc[4]=2;ipc[5]=file_session-0x8000+1;
    send[0]=file_session;gate_dispatch(0x8000,send);assert(!send[0]&&!save_files[file_session-0x8000]);
    char committed_file[300]; snprintf(committed_file, sizeof committed_file, "%s/test", save_path);
    assert(access(committed_file, F_OK) && errno == ENOENT);
    assert(!h_fs_Commit("save"));
    int committed_fd = open(committed_file, O_RDONLY); char committed_bytes[3];
    assert(committed_fd >= 0 && read(committed_fd, committed_bytes, 3) == 3 && !memcmp(committed_bytes, "yes", 3)); close(committed_fd);
    // 写失败即使随后提交也必须保留已提交内容。
    // A failed transaction must preserve committed bytes even when Commit follows.
    assert(!h_fs_OpenFile(&save_file, "save:/test", MODE_WRITE));
    assert(!h_fs_WriteFile(save_file, 0, "bad", 3, NULL)); h_fs_CloseFile(save_file);
    save_poison(ENOSPC); assert(h_fs_Commit("save") != 0);
    committed_fd = open(committed_file, O_RDONLY);
    assert(committed_fd >= 0 && read(committed_fd, committed_bytes, 3) == 3 && !memcmp(committed_bytes, "yes", 3)); close(committed_fd);
    pid_t second_save = fork(); assert(second_save >= 0);
    if (!second_save) { close(probe_save_lock_fd); probe_save_lock_fd = -1; assert(!probe_save_mount()); _exit(0); }
    int second_status; assert(waitpid(second_save, &second_status, 0) == second_save && WIFEXITED(second_status) && !WEXITSTATUS(second_status));
    assert(!rm_tree(dm_save_workdir()) && !rm_tree(save_path)); g_nmnt = 0;
    close(probe_save_lock_fd); probe_save_lock_fd = -1; char lock_path[300]; snprintf(lock_path, sizeof lock_path, "%s.lock", save_path); assert(!unlink(lock_path));
    close(save_root_fd); save_root_fd = -1; unsetenv("DM_PROBE_SAVE_DIR"); assert(!rmdir(save_parent));
    uint32_t zone_session = new_session("timezone"); int64_t timestamp = 1760000000;
    struct tm expected; time_t epoch = timestamp; assert(localtime_r(&epoch, &expected));
    memset(ipc, 0, sizeof ipc); ipc[0] = 6; ipc[1] = 8; ipc[4] = 0x49434653; ipc[6] = 101;
    memcpy(ipc + 8, &timestamp, 8); send[0] = zone_session; gate_dispatch(0x8000, send); assert(!send[0]);
    int16_t year; memcpy(&year, ipc + 8, 2); assert(year == expected.tm_year + 1900);
    assert(((uint8_t *)(ipc + 8))[2] == expected.tm_mon + 1);
    int32_t zone_offset; memcpy(&zone_offset, (uint8_t *)(ipc + 8) + 28, 4); assert(zone_offset == expected.tm_gmtoff);
    assert(munmap(address_space, PROBE_AS_SIZE) == 0);
    puts("PROBE entropy ABI/stability/rejection/regions OK");
}
