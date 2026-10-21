// 真实 CoreAudio 静音缓冲；只模拟暂停跨过截止时间，不伪造消费完成。
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
static long completed_wait(dispatch_semaphore_t semaphore, dispatch_time_t deadline);
static OSStatus enqueue(AudioQueueRef queue, AudioQueueBufferRef buffer, UInt32 count, const AudioStreamPacketDescription *packets);
static OSStatus new_output(const AudioStreamBasicDescription *format, AudioQueueOutputCallback callback, void *context,
                          CFRunLoopRef loop, CFStringRef mode, UInt32 flags, AudioQueueRef *queue);
#define dispatch_semaphore_wait completed_wait
#define AudioQueueEnqueueBuffer enqueue
#define AudioQueueNewOutput new_output
#include "../probe_audio.h"
#undef dispatch_semaphore_wait
#undef AudioQueueEnqueueBuffer
#undef AudioQueueNewOutput

static int stalled, recreate, enqueue_failure, waits, enqueues, creates;
static AudioQueueOutputCallback real_callback;
static int consumed[32], consumed_count;
static void completion(void *context, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    // 只丢弃被注入的完成通知；恢复后的成功仍由真实CoreAudio回调触发。
    if (stalled || (recreate && creates == 1)) return;
    probe_audio_output *output = buffer->mUserData;
    if (output->playing && !atomic_load(&probe_audio_disposing)) {
        assert(consumed_count < 32);
        consumed[consumed_count++] = output->playing->pcm[0];
    }
    real_callback(context, queue, buffer);
}
static OSStatus new_output(const AudioStreamBasicDescription *format, AudioQueueOutputCallback callback, void *context,
                          CFRunLoopRef loop, CFStringRef mode, UInt32 flags, AudioQueueRef *queue) {
    creates++; real_callback = callback;
    return AudioQueueNewOutput(format, completion, context, loop, mode, flags, queue);
}
static OSStatus enqueue(AudioQueueRef queue, AudioQueueBufferRef buffer, UInt32 count, const AudioStreamPacketDescription *packets) {
    if (++enqueues == 1 && enqueue_failure) return kAudioQueueErr_InvalidRunState;
    return AudioQueueEnqueueBuffer(queue, buffer, count, packets);
}
static long completed_wait(dispatch_semaphore_t semaphore, dispatch_time_t deadline) {
    ++waits;
    if (stalled || (recreate && creates == 1) || (!enqueue_failure && waits == 1)) return 1;
    return dispatch_semaphore_wait(semaphore, deadline);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    assert(!strcmp(argv[1], "resume") || !strcmp(argv[1], "recreate") || !strcmp(argv[1], "enqueue") || !strcmp(argv[1], "stalled"));
    stalled = !strcmp(argv[1], "stalled"); recreate = !strcmp(argv[1], "recreate"); enqueue_failure = !strcmp(argv[1], "enqueue");
    int16_t pcm[480] = {0};
    assert(probe_audio_play(NULL, 240, 2, 1) != 0);
    assert(probe_audio_play(pcm, 0, 2, 1) != 0);
    for (int i = 0; i < 24; i++) {
        pcm[0] = (i+1)*10;
        assert(!probe_audio_play(pcm, 240, 2, 1));
        if (stalled || recreate) assert(!atomic_load(&probe_audio_frames));
    }
    pcm[0] = 250;
    int result = probe_audio_play(pcm, 240, 2, 1);
    if (stalled) assert(result != 0 && !atomic_load(&probe_audio_frames));
    else {
        assert(!result);
        for (int i = 0; i < 200 && atomic_load(&probe_audio_frames) < 6000; i++) usleep(10000);
        assert(atomic_load(&probe_audio_frames) == 6000);
    }
    assert(creates == (stalled || recreate || enqueue_failure ? 2 : 1));
    if (stalled) assert(consumed_count == 0);
    else {
        assert(consumed_count == 25);
        for (int i = 0; i < 25; i++) assert(consumed[i] == (i+1)*10);
    }
    atomic_store(&probe_audio_disposing, 1);
    assert(!AudioQueueDispose(probe_audio_queue, true));
    puts("AUDIO completion retry/bounded failure OK");
}
