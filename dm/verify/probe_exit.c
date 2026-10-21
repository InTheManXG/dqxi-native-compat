// 客体 ExitThread 必须先离开客体栈，pthread 的析构不能访问可能已被回收的客体映射。
// ExitThread must leave the guest stack before pthread destructors can race guest unmapping.
#define main probe_main
#include "module_probe.c"
#undef main
#include <assert.h>
#include <stdatomic.h>
static pthread_key_t exit_test_key;
static uint8_t *exit_guest_stack;
static atomic_int destructor_done;
static void exit_destructor(void *unused) {
    (void)unused; char marker;
    uintptr_t stack = (uintptr_t)&marker, base = (uintptr_t)exit_guest_stack;
    assert(stack < base || stack >= base + 0x20000);
    atomic_store(&destructor_done, 1);
}
static void exit_guest(uint64_t unused) {
    (void)unused; assert(!pthread_setspecific(exit_test_key, (void *)1));
    uint64_t regs[31] = {0}; svc_thread(0x0a, regs); abort();
}
int main(void) {
    assert(!pthread_key_create(&g_key18, NULL)); assert(!pthread_key_create(&g_keytls, NULL));
    assert(!pthread_key_create(&g_keytlr, NULL)); assert(!pthread_key_create(&g_keygate, NULL));
    svc_thread_main_init(); assert(!pthread_key_create(&exit_test_key, exit_destructor));
    exit_guest_stack = mmap(NULL, 0x20000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(exit_guest_stack != MAP_FAILED);
    uint64_t regs[31] = {0, (uint64_t)exit_guest, 0, (uint64_t)exit_guest_stack + 0x20000, 44, 0};
    assert(svc_thread(8, regs) && !regs[0]); uint32_t handle = regs[1];
    regs[0] = handle; assert(svc_thread(9, regs) && !regs[0]);
    for(int i=0;i<5000&&!atomic_load(&destructor_done);i++) usleep(1000);
    assert(atomic_load(&destructor_done));
    regs[0] = handle; assert(svc_thread(0x16, regs) && !regs[0]);
    assert(!munmap(exit_guest_stack, 0x20000)); puts("PROBE thread exit restores host stack OK");
}
