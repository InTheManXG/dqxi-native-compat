// 直接测试真实 HLE 解析器：弱符号缺失、已有强导入桩、宿主提供定义三条路径。
// Test the real HLE resolver for missing weak symbols, existing strong-import stubs and host definitions.
#include "../host/hle.h"
#include <assert.h>
struct dm_flags g_flag;
uint64_t unimpl_stubs[4096];
void *hlecount_wrap(const char *kind, const char *name, void *fn) { (void)kind; (void)name; return fn; }
uint64_t resolve_import(module_t *, const char *, int);
HLE("test_host_optional", int, host_optional, void) { return 17; }
int main(void) {
    module_t m = {0};
    assert(resolve_import(&m, "missing", 1) == 0);
    uint64_t strong = resolve_import(&m, "missing", 0);
    assert(strong == (uint64_t)&unimpl_stubs[0]);
    assert(resolve_import(&m, "missing", 0) == strong);
    assert(resolve_import(&m, "missing", 1) == 0);
    assert(resolve_import(&m, "test_host_optional", 1) == (uint64_t)host_optional);
    assert(resolve_import(&m, "test_host_optional", 0) == (uint64_t)host_optional);
    puts("IMPORT weak/strong/host-definition OK");
}
