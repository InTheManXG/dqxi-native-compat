// 持续5毫秒PCM必须产生真实输出电平；仅完成回调不代表可听输出。
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <mach/mach.h>
#include <dispatch/dispatch.h>
#include <assert.h>
static uint32_t audio_event_write;
static int svc_thread(int command, uint64_t *regs) { (void)command; regs[0] = 0; return 1; }
static void die(const char *fmt, ...) { (void)fmt; abort(); }
#include <AudioToolbox/AudioToolbox.h>
static _Atomic int stream_started, silent_during_stream;
static OSStatus tracked_enqueue(AudioQueueRef q, AudioQueueBufferRef b, UInt32 n, const AudioStreamPacketDescription *p);
#define AudioQueueEnqueueBuffer tracked_enqueue
#include "../probe_audio.h"
#undef AudioQueueEnqueueBuffer
static OSStatus tracked_enqueue(AudioQueueRef q, AudioQueueBufferRef b, UInt32 n, const AudioStreamPacketDescription *p) {
    if (stream_started && !((probe_audio_output *)b->mUserData)->playing) silent_during_stream++;
    return AudioQueueEnqueueBuffer(q,b,n,p);
}
static uint64_t test_time(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;
}
int main(void) {
    int16_t pcm[480]; int nonzero = 0, resumed = 0; float peak = 0;
    uint64_t deadline = test_time();
    for (int k = 0; k < 400; k++) {
        // 每隔240ms引入20ms延迟，随后按绝对时钟追赶，平均采样率不变。
        if (k > 160 && k % 48 == 30) usleep(20000);
        for (int i = 0; i < 240; i++)
            pcm[2*i] = pcm[2*i+1] = (int16_t)(400 * sin(2 * 3.141592653589793 * 440 * (k*240+i) / 48000.));
        assert(!probe_audio_play(pcm, 240, 2, 1));
        if (!k) {
            UInt32 on = 1;
            assert(!AudioQueueSetProperty(probe_audio_queue, kAudioQueueProperty_EnableLevelMetering, &on, 4));
        }
        AudioQueueLevelMeterState meters[2] = {{0}}; UInt32 size = sizeof(meters);
        assert(!AudioQueueGetProperty(probe_audio_queue, kAudioQueueProperty_CurrentLevelMeter, meters, &size));
        if (meters[0].mPeakPower > 0) nonzero++;
        if (meters[0].mPeakPower > peak) peak = meters[0].mPeakPower;
        if (k > 130 && meters[0].mPeakPower > 0) resumed++;
        assert(atomic_load(&probe_audio_frames) <= (uint64_t)(k+1)*240);
        // 模拟渲染器5ms供给及短暂停顿，恢复后仍须有真实电平。
        if (k == 160) { silent_during_stream = 0; stream_started = 1; }
        // 六段一批模拟调度合并回调；平均仍是48kHz，不能每批之间插静音。
        if (k == 120) { usleep(100000); deadline = test_time(); }
        if (k % 6 == 5) {
            deadline += 30000000;
            uint64_t now = test_time();
            if (now < deadline) usleep((useconds_t)((deadline-now)/1000));
        }
    }
    stream_started = 0;
    fprintf(stderr,"silent during paced stream=%d\n",silent_during_stream);
    assert(silent_during_stream <= 6);
    printf("AUDIO stream nonzero=%d peak=%f frames=%llu\n", nonzero, peak, (unsigned long long)atomic_load(&probe_audio_frames));
    assert(nonzero > 20 && resumed > 20 && peak > 0.001f);
    for (int i = 0; i < 200 && atomic_load(&probe_audio_frames) < 96000; i++) usleep(10000);
    assert(atomic_load(&probe_audio_frames) == 96000);
    atomic_store(&probe_audio_disposing, 1);
    assert(!AudioQueueDispose(probe_audio_queue, true));
    return 0;
}
