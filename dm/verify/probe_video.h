// VP9 使用宿主 libvpx，解复用、音频和时间线仍由游戏的媒体库执行。
// VP9 uses host libvpx; guest media code retains demux, audio and presentation clocks.
static int probe_use_native_vp9(const char *mime, int encoder, const char *match) {
    return !encoder && !match && !strcmp(mime, "video/x-vnd.on2.vp9");
}
#ifdef DM_PROBE_METAL
#include "../host/hle.h"
#define PROBE_CODEC_MATCH "_ZN7android8OMXCodec18findMatchingCodecsEPKcbS2_jPNS_6VectorINS_7String8EEE"
HLE_FORCE(PROBE_CODEC_MATCH, "select native software VP9 without NVIDIA devices", void, probe_codec_match,
          const char *mime, int encoder, const char *match, unsigned flags, void *matches) {
    void (*original)(const char *, int, const char *, unsigned, void *) = NULL;
    for (int i = 0; i < g_nmod && !original; i++) original = module_sym(&g_mod[i], PROBE_CODEC_MATCH);
    if (!original) die("probe original codec selector missing");
    original(mime, encoder, match, flags, matches);
    if (probe_use_native_vp9(mime, encoder, match)) {
        LOG("PROBE native VP9 software decoder selected");
        void (*clear)(void *) = guest_sym("_ZN7android10VectorImpl5clearEv");
        void (*string_init)(void *, const char *) = guest_sym("_ZN7android7String8C1EPKc");
        void (*string_free)(void *) = guest_sym("_ZN7android7String8D1Ev");
        long (*push)(void *, const void *) = guest_sym("_ZN7android10VectorImpl4pushEPKv");
        if (!clear || !string_init || !string_free || !push) die("probe codec vector ABI unavailable");
        uint64_t name = 0; clear(matches); string_init(&name, "OMX.google.vp9.decoder");
        long index = push(matches, &name); string_free(&name);
        if (index < 0) die("probe codec selection allocation failed");
    }
}

#include <dlfcn.h>
typedef struct probe_video_entry { void *component; struct probe_video_entry *next; } probe_video_entry;
static probe_video_entry *probe_video_components;
static pthread_mutex_t probe_video_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t (*probe_vp9_create_fn)(const void *, void *, void **);
static uint32_t (*probe_vp9_destroy_fn)(void *);
#define PROBE_OMX_CREATE "_ZN7android9OMXMaster21makeComponentInstanceEPKcPK16OMX_CALLBACKTYPEPvPP17OMX_COMPONENTTYPE"
HLE_FORCE(PROBE_OMX_CREATE, "native VP9 component bridge", uint32_t, probe_omx_create,
          void *master, const char *name, const void *callbacks, void *app, void **out) {
    if (strcmp(name, "OMX.google.vp9.decoder")) {
        uint32_t (*original)(void *, const char *, const void *, void *, void **) = guest_sym(PROBE_OMX_CREATE);
        if (!original) die("probe OMX factory missing");
        return original(master, name, callbacks, app, out);
    }
    // 模式选择会同时播放两个预览；分别登记实例，避免把原生组件交给客体析构器。
    probe_video_entry *entry = malloc(sizeof *entry);
    if (!entry) return 0x80001000u; // OMX_ErrorInsufficientResources
    pthread_mutex_lock(&probe_video_lock);
    if (!probe_vp9_create_fn) {
        const char *path = getenv("DM_PROBE_VIDEO_LIB");
        void *library = path ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : NULL;
        if (!library) die("probe native VP9 library missing: %s", dlerror());
        probe_vp9_create_fn = dlsym(library, "probe_vp9_create"); probe_vp9_destroy_fn = dlsym(library, "probe_vp9_destroy");
        if (!probe_vp9_create_fn || !probe_vp9_destroy_fn) die("probe VP9 bridge ABI missing");
    }
    uint32_t result = probe_vp9_create_fn(callbacks, app, out);
    if (!result) { entry->component = *out; entry->next = probe_video_components; probe_video_components = entry; }
    else free(entry);
    pthread_mutex_unlock(&probe_video_lock);
    return result;
}
#define PROBE_OMX_DESTROY "_ZN7android9OMXMaster24destroyComponentInstanceEP17OMX_COMPONENTTYPE"
HLE_FORCE(PROBE_OMX_DESTROY, "release native VP9 component", uint32_t, probe_omx_destroy, void *master, void *component) {
    pthread_mutex_lock(&probe_video_lock);
    for (probe_video_entry **p = &probe_video_components; *p; p = &(*p)->next) {
        if ((*p)->component != component) continue;
        uint32_t result = probe_vp9_destroy_fn(component);
        // 缓冲尚未归还时析构会失败，保留登记以便调用方清理后重试。
        if (!result) { probe_video_entry *entry = *p; *p = entry->next; free(entry); }
        pthread_mutex_unlock(&probe_video_lock);
        return result;
    }
    pthread_mutex_unlock(&probe_video_lock);
    uint32_t (*original)(void *, void *) = guest_sym(PROBE_OMX_DESTROY);
    if (!original) die("probe OMX destructor missing"); return original(master, component);
}

#endif
