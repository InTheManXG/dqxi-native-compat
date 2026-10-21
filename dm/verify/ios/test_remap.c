// 在 Mac 执行同一套 iOS 内嵌映射代码，验证签名代码页、前置跳转和可写数据互不混淆。
#include <TargetConditionals.h>
#undef TARGET_OS_IPHONE
#define TARGET_OS_IPHONE 1
#include "../../host/loader.c"
#include <assert.h>
int main(void){
    module_t m={.image_off=0x4000,.text_end=0x4000,.key_page_off=0x8000,.key_page_size=0x4000};
    strcpy(m.name,"main");for(int i=0;i<8;i++)m.key_idx[i]=i;
    create_keys();map_image(&m,NULL);
    assert(m.size==0xc000);
    assert(((int (*)(void))(m.base-0x4000))()==42);
    assert(((int (*)(void))m.base)()==42);
    *(uint32_t *)(m.base+0x4000)=123;
    assert(*(uint32_t *)(m.base+0x4000)==123);
    assert(*(uint64_t *)(m.base+0x8000)==g_key18*8);
    puts("embedded mapping: signed prefix branch + original text execution + writable data + key page OK");
}
