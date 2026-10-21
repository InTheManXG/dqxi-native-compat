// 独立音频渲染库的原生边界：只接收 PCM、事件和已映射内存，不执行客体 CPU 指令。
// Native audio-renderer boundary: PCM, events and mapped memory only; no guest CPU execution.
#pragma once
#include <AudioToolbox/AudioToolbox.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <math.h>
#include <stdatomic.h>
#include <time.h>
static uint64_t (*audio_size_native)(const void *);
static uint32_t (*audio_open_native)(const void *, uint64_t, uint64_t);
static uint32_t (*audio_update_native)(const void *, int, void *, int, void *, int);
static uint32_t (*audio_control_native)(int, uint32_t, uint32_t *);
static AudioQueueRef probe_audio_queue;
static void (*audio_consumed_native)(int);
// 二十四段PCM提供有界背压；短暂预缓冲吸收调度抖动，避免5ms小段之间反复插静音。
// state: 0空闲、1待播放、2硬件缓冲中；只有真实完成才能从2回到0。
typedef struct { int state, frames; uint64_t order; int16_t pcm[8192]; } probe_audio_slot;
typedef struct { AudioQueueBufferRef buffer; probe_audio_slot *playing; } probe_audio_output;
static probe_audio_slot probe_audio_slots[24];
// 六段5ms输出覆盖约30ms调度余量，避免回调批次接近15ms旧队列长度时产生爆音。
static probe_audio_output probe_audio_outputs[6];
static pthread_mutex_t probe_audio_pcm_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t probe_audio_order;
static uint64_t probe_audio_resume_at;
static uint64_t probe_audio_time(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static _Atomic int probe_audio_disposing, probe_audio_failed;
static dispatch_semaphore_t probe_audio_done;
static pthread_mutex_t probe_audio_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t probe_audio_frames;
static int probe_audio_validate(uint64_t address, uint64_t size, int write) {
    if (!size) return 1;
    if (!address || size > UINT64_MAX - address) return 0;
    uint64_t end = address + size;
    while (address < end) {
        vm_address_t base = address; vm_size_t bytes = 0;
        vm_region_basic_info_data_64_t info; mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t object = 0;
        kern_return_t result = vm_region_64(mach_task_self(), &base, &bytes, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
        if (object) mach_port_deallocate(mach_task_self(), object);
        if (result != KERN_SUCCESS || base > address || !bytes || !(info.protection & VM_PROT_READ) || (write && !(info.protection & VM_PROT_WRITE))) return 0;
        address = base + bytes;
    }
    return 1;
}
static void probe_audio_notify(int signaled) {
    uint64_t regs[31] = {audio_event_write};
    if (audio_event_write && (!svc_thread(signaled ? 0x11 : 0x12, regs) || regs[0])) die("probe audio event failed");
}
static void probe_audio_fill(probe_audio_output *output) {
    // 调用者持PCM锁；按序号取最早一段，不能依赖哪个硬件槽先回调。
    probe_audio_slot *next = NULL;
    for (int i = 0; i < 24; i++) {
        probe_audio_slot *slot = &probe_audio_slots[i];
        if (slot->state == 1 && (!next || slot->order < next->order)) next = slot;
    }
    // 首次/欠载后积累60ms余量；不足一批的短音效也会在截止时间后正常播放。
    uint64_t now = probe_audio_time();
    if (!next) probe_audio_resume_at = now + 60000000;
    else if (now < probe_audio_resume_at) next = NULL;
    output->playing = next;
    if (next) {
        next->state = 2;
        output->buffer->mAudioDataByteSize = next->frames * 4;
        memcpy(output->buffer->mAudioData, next->pcm, next->frames * 4);
    } else {
        output->buffer->mAudioDataByteSize = 240 * 4;
        memset(output->buffer->mAudioData, 0, 240 * 4);
    }
}
static void probe_audio_consumed(void *context, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    (void)context;
    // 同步销毁触发的退还不算播放完成；恢复时重排仍在播放的真实PCM。
    if (atomic_load(&probe_audio_disposing)) return;
    probe_audio_output *output = buffer->mUserData;
    pthread_mutex_lock(&probe_audio_pcm_mutex);
    int frames = output->playing ? output->playing->frames : 0;
    if (output->playing) output->playing->state = 0;
    probe_audio_fill(output);
    pthread_mutex_unlock(&probe_audio_pcm_mutex);
    if (frames) {
        atomic_fetch_add(&probe_audio_frames, frames);
        if (audio_consumed_native) audio_consumed_native(frames);
        dispatch_semaphore_signal(probe_audio_done);
    }
    OSStatus status = AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);
    if (status) { atomic_store(&probe_audio_failed, status); dispatch_semaphore_signal(probe_audio_done); }
}
static OSStatus probe_audio_create(void) {
    AudioStreamBasicDescription format = {.mSampleRate = 48000, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
        .mBytesPerPacket = 4, .mFramesPerPacket = 1, .mBytesPerFrame = 4, .mChannelsPerFrame = 2, .mBitsPerChannel = 16};
    probe_audio_done = dispatch_semaphore_create(0);
    OSStatus status = AudioQueueNewOutput(&format, probe_audio_consumed, NULL, NULL, NULL, 0, &probe_audio_queue);
    if (status) return status;
    for (int i = 0; i < 6; i++) {
        probe_audio_output *output = &probe_audio_outputs[i];
        if ((status = AudioQueueAllocateBuffer(probe_audio_queue, 4096 * 4, &output->buffer))) return status;
        output->buffer->mUserData = output;
        pthread_mutex_lock(&probe_audio_pcm_mutex);
        probe_audio_fill(output);
        pthread_mutex_unlock(&probe_audio_pcm_mutex);
        if ((status = AudioQueueEnqueueBuffer(probe_audio_queue, output->buffer, 0, NULL))) return status;
    }
    status = AudioQueueStart(probe_audio_queue, NULL);
    return status;
}
static int probe_audio_play(const int16_t *samples, int frames, int channels, float volume) {
    if (!samples || frames <= 0 || frames > 4096 || channels != 2 || !isfinite(volume)) return -1;
    pthread_mutex_lock(&probe_audio_mutex);
    int result = -1;
    for (int attempt = 0; attempt < 2; attempt++) {
        OSStatus status = 0; const char *phase = "create/replay";
        if (!probe_audio_queue && (status = probe_audio_create())) goto recover;
        for (;;) {
            if ((status = atomic_load(&probe_audio_failed))) { phase = "callback enqueue"; goto recover; }
            pthread_mutex_lock(&probe_audio_pcm_mutex);
            probe_audio_slot *slot = NULL;
            for (int i = 0; i < 24; i++) if (!probe_audio_slots[i].state) { slot = &probe_audio_slots[i]; break; }
            if (slot) {
                for (int i = 0; i < frames * channels; i++) {
                    float sample = samples[i] * volume;
                    slot->pcm[i] = (int16_t)(sample > 32767 ? 32767 : sample < -32768 ? -32768 : sample);
                }
                slot->frames = frames; slot->order = ++probe_audio_order; slot->state = 1;
            }
            pthread_mutex_unlock(&probe_audio_pcm_mutex);
            if (slot) { result = 0; goto done; }
            if (dispatch_semaphore_wait(probe_audio_done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) &&
                dispatch_semaphore_wait(probe_audio_done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC))) {
                phase = "completion timeout"; status = -1; goto recover;
            }
        }
     recover:
        fprintf(stderr, "[probe-audio] %s status=%d attempt=%d\n", phase, (int)status, attempt + 1);
        if (attempt) break;
        atomic_store(&probe_audio_disposing, 1);
        if (probe_audio_queue && (status = AudioQueueDispose(probe_audio_queue, true))) {
            fprintf(stderr, "[probe-audio] dispose status=%d\n", (int)status); goto done;
        }
        // Dispose等待回调退出后，未完成PCM全部回到待播放状态，旧回调不会确认新缓冲。
        for (int i = 0; i < 24; i++) if (probe_audio_slots[i].state == 2) probe_audio_slots[i].state = 1;
        atomic_store(&probe_audio_disposing, 0); atomic_store(&probe_audio_failed, 0);
        probe_audio_queue = NULL;
        if (probe_audio_done) dispatch_release(probe_audio_done);
        probe_audio_done = NULL;
    }
 done:
    pthread_mutex_unlock(&probe_audio_mutex); return result;
}
// 客体在 SDK 栈上执行；AOT 的 GC/异常展开要求真实 pthread 栈，所有入口须转给固定宿主线程。
// Guest code uses an SDK stack; AOT GC/unwinding requires a real pthread stack for every entry.
typedef struct { int kind; uint64_t args[6], result; int pending; } audio_job;
static audio_job probe_audio_job;
static pthread_mutex_t probe_audio_job_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t probe_audio_job_condition = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t probe_audio_call_mutex = PTHREAD_MUTEX_INITIALIZER;
static int probe_audio_loaded;
static void *probe_audio_worker(void *unused) {
    (void)unused; pthread_setname_np("dqxi-native-audio");
    pthread_mutex_lock(&probe_audio_job_mutex);
    for (;;) {
        while (!probe_audio_job.pending) pthread_cond_wait(&probe_audio_job_condition, &probe_audio_job_mutex);
        audio_job job = probe_audio_job; pthread_mutex_unlock(&probe_audio_job_mutex);
        uint64_t *a = job.args, result = 0;
        if (job.kind == 0) {
            void *library = dlopen((const char *)a[0], RTLD_NOW | RTLD_LOCAL);
            if (!library) die("probe audio library: %s", dlerror());
            int (*initialize)(void *, void *, void *) = dlsym(library, "dq_audio_init");
            audio_consumed_native = dlsym(library, "dq_audio_consumed");
            audio_size_native = dlsym(library, "dq_audio_size"); audio_open_native = dlsym(library, "dq_audio_open");
            audio_update_native = dlsym(library, "dq_audio_update"); audio_control_native = dlsym(library, "dq_audio_control");
            // 音频库的默认日志器会接管 stderr；初始化后恢复宿主描述符，保留探针错误日志。
            // The library logger redirects stderr; restore the host descriptor after initialization.
            int saved_stderr = dup(STDERR_FILENO); if (saved_stderr < 0) die("probe audio preserve stderr");
            if (!initialize || !audio_consumed_native || !audio_size_native || !audio_open_native || !audio_update_native || !audio_control_native ||
                initialize(probe_audio_validate, probe_audio_notify, probe_audio_play)) die("probe audio bridge ABI/init");
            if (dup2(saved_stderr, STDERR_FILENO) < 0) die("probe audio restore stderr"); close(saved_stderr);
            result = 1;
        } else if (job.kind == 1) result = audio_size_native((const void *)a[0]);
        else if (job.kind == 2) result = audio_open_native((const void *)a[0], a[1], a[2]);
        else if (job.kind == 3) result = audio_update_native((const void *)a[0], a[1], (void *)a[2], a[3], (void *)a[4], a[5]);
        else if (job.kind == 4) result = audio_control_native(a[0], a[1], (void *)a[2]);
        else die("probe audio invalid job");
        pthread_mutex_lock(&probe_audio_job_mutex);
        probe_audio_job.result = result; probe_audio_job.pending = 0; pthread_cond_broadcast(&probe_audio_job_condition);
    }
    return NULL;
}
static uint64_t probe_audio_call(audio_job job) {
    pthread_mutex_lock(&probe_audio_call_mutex); pthread_mutex_lock(&probe_audio_job_mutex);
    probe_audio_job = job; probe_audio_job.pending = 1; pthread_cond_broadcast(&probe_audio_job_condition);
    while (probe_audio_job.pending) pthread_cond_wait(&probe_audio_job_condition, &probe_audio_job_mutex);
    uint64_t result = probe_audio_job.result;
    pthread_mutex_unlock(&probe_audio_job_mutex); pthread_mutex_unlock(&probe_audio_call_mutex); return result;
}
static int probe_audio_load(void) {
    if (probe_audio_loaded) return 1;
    const char *path = getenv("DM_PROBE_AUDIO_LIB"); if (!path) return 0;
    pthread_t worker; pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 8 << 20);
    if (pthread_create(&worker, &attr, probe_audio_worker, NULL)) die("probe audio worker");
    pthread_attr_destroy(&attr); pthread_detach(worker);
    probe_audio_loaded = probe_audio_call((audio_job){.kind=0, .args={(uint64_t)path}}); return probe_audio_loaded;
}
static uint64_t probe_audio_size(const void *p) { return probe_audio_call((audio_job){.kind=1, .args={(uint64_t)p}}); }
static uint32_t probe_audio_open(const void *p, uint64_t address, uint64_t size) {
    return probe_audio_call((audio_job){.kind=2, .args={(uint64_t)p,address,size}});
}
static uint32_t probe_audio_update(const void *in, int ni, void *out, int no, void *perf, int np) {
    return probe_audio_call((audio_job){.kind=3, .args={(uint64_t)in,ni,(uint64_t)out,no,(uint64_t)perf,np}});
}
static uint32_t probe_audio_control(int command, uint32_t value, uint32_t *out) {
    return probe_audio_call((audio_job){.kind=4, .args={command,value,(uint64_t)out}});
}
