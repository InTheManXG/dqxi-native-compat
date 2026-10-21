// 直接走实际桥接层，验证并存组件、失败重试及非 VP9 组件的析构路由。
#include <assert.h>
#include <string.h>
#include <OMX_Component.h>
#define DM_PROBE_METAL 1
#include "../probe_video.h"
module_t g_mod[MAX_MOD];
int g_nmod;
static int fallback_calls;
static uint32_t fallback(void *master, void *component) {
    assert(master == (void *)1 && component == (void *)2);
    fallback_calls++;return 123;
}
void *module_sym(module_t *m, const char *name) { (void)m;(void)name;return NULL; }
void *guest_sym(const char *name) { assert(!strcmp(name,PROBE_OMX_DESTROY));return fallback; }
static OMX_ERRORTYPE event(OMX_HANDLETYPE h,void *a,OMX_EVENTTYPE e,OMX_U32 x,OMX_U32 y,void *z) {
    (void)h;(void)a;(void)e;(void)x;(void)y;(void)z;return 0;
}
static OMX_ERRORTYPE buffer(OMX_HANDLETYPE h,void *a,OMX_BUFFERHEADERTYPE *b) { (void)h;(void)a;(void)b;return 0; }
int main(void) {
    OMX_CALLBACKTYPE cb={event,buffer,buffer};
    OMX_COMPONENTTYPE *a,*b;
    assert(!probe_omx_create(NULL,"OMX.google.vp9.decoder",&cb,NULL,(void **)&a));
    assert(!probe_omx_create(NULL,"OMX.google.vp9.decoder",&cb,NULL,(void **)&b));
    assert(a!=b);
    OMX_BUFFERHEADERTYPE *held;
    assert(!a->AllocateBuffer(a,&held,0,NULL,4*1024*1024));
    assert(probe_omx_destroy(NULL,a)==OMX_ErrorIncorrectStateOperation);
    assert(!probe_omx_destroy(NULL,b));
    OMX_STATETYPE state;
    assert(!a->GetState(a,&state)&&state==OMX_StateLoaded);
    assert(!a->FreeBuffer(a,0,held));
    assert(!probe_omx_destroy(NULL,a));
    assert(probe_video_components==NULL);
    assert(probe_omx_destroy((void *)1,(void *)2)==123&&fallback_calls==1);
    puts("VP9 bridge: concurrent lifetimes, destroy retry and guest fallback passed");
}
