// 仅复用文件后端，不登记 nn::fs 接管项，也不调用暗黑专用存档初始化。
// Reuse the filesystem backend without symbol interception or D2R save seeding.
#include "../host/hle.h"
#pragma push_macro("HLE_FORCE")
#undef HLE_FORCE
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#include "../host/hle_fs.c"
#pragma pop_macro("HLE_FORCE")

#include <sys/file.h>
static int probe_save_lock_fd = -1;
static int probe_save_mount(void) {
    const char *path = getenv("DM_PROBE_SAVE_DIR");
    if (!path || path[0] != '/' || strlen(path) >= sizeof g_mnt[0].commit - 1 || path[strlen(path) - 1] == '/') return 0;
    // 锁文件不参与目录交换，防止双开实例清理彼此尚未提交的工作副本。
    // Keep the lock outside swapped directories so concurrent instances cannot erase live work.
    if (probe_save_lock_fd < 0) {
        char lock_path[1100]; snprintf(lock_path, sizeof lock_path, "%s.lock", path);
        int fd = open(lock_path, O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
        if (fd < 0) return 0;
        if (flock(fd, LOCK_EX | LOCK_NB)) { close(fd); return 0; }
        probe_save_lock_fd = fd;
    }
    // 拒绝事务同级目录的符号链接，避免旧工作目录清理越出独立存档根。
    // Reject symlink transaction siblings before cleanup can follow them.
    const char *suffixes[] = {"", ".work", ".new", ".work.re"};
    for (unsigned i = 0; i < 4; i++) {
        char sibling[1100]; struct stat st;
        snprintf(sibling, sizeof sibling, "%s%s", path, suffixes[i]);
        if (!lstat(sibling, &st)) { if (!S_ISDIR(st.st_mode)) return 0; }
        else if (errno != ENOENT) return 0;
    }
    return !mount_ex("save", path, 0, 1) && dm_save_workdir();
}

static int probe_save_path(const uint32_t *desc, char path[0x310]) {
    uint64_t address = desc[1] | ((uint64_t)(desc[0] & 0xf000) << 20) | ((uint64_t)(desc[0] & 0x1c0) << 30);
    size_t bytes = desc[0] >> 16; char guest[0x301]; vm_size_t got;
    if ((desc[0] & 0xe3f) || !bytes || bytes > sizeof guest || !probe_ipc_address(&address, bytes) ||
        vm_read_overwrite(mach_task_self(), address, bytes, (vm_address_t)guest, &got) != KERN_SUCCESS || got != bytes ||
        !memchr(guest, 0, bytes) || guest[0] != '/' || strchr(guest, ':')) return 0;
    // 客体路径必须留在挂载内；拒绝 .. 与符号链接，不能借存档接口访问宿主文件。
    // Guest paths must remain inside the mount, without parent traversal or symlinks.
    snprintf(path, 0x310, "save:%s", guest);
    char physical[1100]; strlcpy(physical, dm_save_workdir(), sizeof physical);
    char *rest = guest + 1, *part; int missing = 0;
    while ((part = strsep(&rest, "/"))) {
        if (!strcmp(part, "..")) return 0;
        if (!*part || !strcmp(part, ".") || missing) continue;
        if (!path_component(physical, sizeof physical, part, 1)) return 0;
        struct stat st;
        if (!lstat(physical, &st)) { if (S_ISLNK(st.st_mode)) return 0; }
        else if (errno == ENOENT) missing = 1;
        else return 0;
    }
    return 1;
}
