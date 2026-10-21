// 用真实 Mac 装载路径执行自造 NSO；检查前置区权限、基址和线程槽，避免只验证文件生成。
// Execute synthetic NSOs through the real Mac loader; verify prefix permissions, module bases and thread slots.
#include "../host/loader.c"
#include <assert.h>
#include <mach/mach.h>

static int registered;
struct dm_flags g_flag;
void *hlecount_wrap(const char *kind, const char *name, void *fn) {
    (void)kind; (void)name; return fn;
}
uint64_t resolve_import(module_t *m, const char *name, int weak) {
    if (weak) { assert(!strcmp(name, "missing")); return 0; }
    (void)m; (void)name; assert(!"不应调用宿主导入桩"); return 0;
}
void gate_register(uint64_t ret, const char *kind, int arg, uint64_t orig) {
    assert(module_of((void *)ret) == &g_mod[0]);
    assert(!strcmp(kind, "svc") && arg == 1 && orig == 0x200);
    assert(*(uint32_t *)ret == 0xa8c17bfd);
    registered++;
}

static vm_prot_t protection(void *p) {
    vm_address_t address = (vm_address_t)p;
    vm_size_t size;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object;
    assert(vm_region_64(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
                       (vm_region_info_t)&info, &count, &object) == KERN_SUCCESS);
    if (object) mach_port_deallocate(mach_task_self(), object);
    return info.protection;
}

int main(int argc, char **argv) {
    assert(argc == 2 || argc == 3);
    module_t *m = &g_mod[0];
    strcpy(m->name, "test");
    read_layout(m, argv[1]);
    create_keys();
    map_image(m, argv[1]);
    g_nmod = 1; register_gates(m, argv[1]); assert(registered == 1);
    // 相对、本模块符号、跨模块符号三种重定位都必须仍以原 NSO 的 base 为准。
    // Relative, local-symbol and cross-module relocations must still use the original NSO base.
    Elf64_Sym local[4] = {{0}, {.st_name=1, .st_shndx=1, .st_value=0x100}, {.st_name=7, .st_info=0x20}, {.st_name=12, .st_info=0x20}};
    Elf64_Sym peer[2] = {{0}, {.st_name=7, .st_shndx=1, .st_value=0x40}};
    uint8_t peer_image[128];
    m->symtab=local; m->strtab="\0local\0peer\0missing"; m->nsym=4;
    g_mod[1].symtab=peer; g_mod[1].strtab=m->strtab; g_mod[1].nsym=2; g_mod[1].base=peer_image;
    g_nmod=2;
    Elf64_Rela rels[4] = {{m->data_off+0x100, R_AARCH64_RELATIVE, 0x80},
                         {m->data_off+0x108, (1ull<<32)|R_AARCH64_ABS64, 4},
                         {m->data_off+0x110, (2ull<<32)|R_AARCH64_JUMP_SLOT, 0},
                         {m->data_off+0x118, (3ull<<32)|R_AARCH64_ABS64, 7}};
    memcpy(m->base+m->data_off, rels, sizeof rels);
    Elf64_Dyn dyn[] = {{DT_RELA, {.d_ptr=m->data_off}}, {DT_RELASZ, {.d_val=sizeof rels}}, {0}};
    m->dyn=dyn; relocate(m);
    assert(*(uint64_t *)(m->base+m->data_off+0x100) == (uintptr_t)m->base+0x80);
    assert(*(uint64_t *)(m->base+m->data_off+0x108) == (uintptr_t)m->base+0x104);
    assert(*(uint64_t *)(m->base+m->data_off+0x110) == (uintptr_t)peer_image+0x40);
    // 缺失弱符号必须为零，ABS64 仍叠加加数。 | Missing weak symbols are zero; ABS64 still applies its addend.
    assert(*(uint64_t *)(m->base+m->data_off+0x118) == 7);
    assert(!memcmp(m->base + 0x40, "MOD0", 4));
    assert(protection(m->base) == (VM_PROT_READ | VM_PROT_EXECUTE));
    assert(protection(m->base + m->data_off) == (VM_PROT_READ | VM_PROT_WRITE));
    assert(protection(m->base + m->key_page_off) == VM_PROT_READ);
    if (m->image_off)
        assert(protection(m->base - m->image_off) == (VM_PROT_READ | VM_PROT_EXECUTE));
    // 客体 mov x18,x0; ret 必须写进 K18，不能把 Apple 保留的 x18 当游戏寄存器。
    // Guest mov x18,x0; ret must update K18, preserving the Apple-reserved physical x18.
    if (argc == 3) {
        for (int alternate = 0; alternate < 2; alternate++) {
            for (int i = 0; i < 1000; i++) {
                uint64_t cell = 0;
                pthread_setspecific(g_key18, (void *)0x1234);
                ((void (*)(uint64_t *, int))(m->base + 0x80))(&cell, alternate);
                assert(cell == 0 && !pthread_getspecific(g_key18));
                cell = 1; pthread_setspecific(g_key18, (void *)0x1234);
                ((void (*)(uint64_t *, int))(m->base + 0x80))(&cell, alternate);
                assert(cell == 1 && (uintptr_t)pthread_getspecific(g_key18) == 0x1234);
            }
        }
    } else {
        ((void (*)(uintptr_t))(m->base + 0x100))(0x12345678);
        assert((uintptr_t)pthread_getspecific(g_key18) == 0x12345678);
    }
    munmap(m->base - m->image_off, m->size + m->image_off);
    puts("LOADER layout/base/permissions/native-x18 OK");
}
