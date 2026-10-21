// 合成 REV7 静音配置验证真实 CoreAudio 消费和完成事件，不含游戏数据。
// Synthetic REV7 silence validates real CoreAudio consumption and completion events without game data.
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <mach/mach.h>
#include <assert.h>
static uint32_t audio_event_write=1;
static _Atomic unsigned signals;
static int svc_thread(int command,uint64_t *regs){if(command==0x11)atomic_fetch_add(&signals,1);regs[0]=0;return 1;}
static void die(const char *fmt,...){va_list args;va_start(args,fmt);vfprintf(stderr,fmt,args);va_end(args);abort();}
#include "../probe_audio.h"
int main(int argc,char **argv){
 assert(argc==2);setenv("DM_PROBE_AUDIO_LIB",argv[1],1);assert(probe_audio_load());
 uint32_t config[13]={48000,240,2,0,1,1,0,0,0,0,0,0,0x37564552};
 uint64_t size=probe_audio_size(config);assert(size && size<0x1000000);void *work=calloc(1,size);assert(work);
 assert(!probe_audio_open(config,(uint64_t)work,size));
 uint32_t lengths[8]={16,128,368,112,0,2384,320,16};size_t total=64;for(int i=0;i<8;i++)total+=lengths[i];
 uint8_t *input=calloc(1,total);uint32_t *header=(void*)input;header[0]=config[12];memcpy(header+1,lengths,sizeof lengths);header[15]=total;
 uint32_t *beh=(void*)(input+64);beh[0]=config[12];
 uint8_t *mix=input+64+16+128+368+112;
 *(uint32_t*)mix=0xcafebabe;*(uint32_t*)(mix+4)=1;mix+=32;
 *(float*)mix=1;*(uint32_t*)(mix+4)=48000;*(uint32_t*)(mix+8)=2;mix[12]=1;mix[13]=1;*(uint32_t*)(mix+0x924)=0x7fffffff;*(uint32_t*)(mix+0x928)=0xffffffff;
 uint8_t *sink=input+64+16+128+368+112+2384;sink[0]=1;sink[1]=1;memcpy(sink+32,"MainAudioOut",13);*(uint32_t*)(sink+288)=2;sink[292]=0;sink[293]=1;
 uint8_t output[1024]={0};uint32_t r=probe_audio_update(input,total,output,sizeof output,NULL,0);printf("update result=%#x size=%llu\n",r,size);assert(!r);
 uint32_t value=0;assert(!probe_audio_control(5,0,&value));
 for(int k=0;k<1200 && (!atomic_load(&probe_audio_frames) || !atomic_load(&signals));k++)usleep(10000);
 assert(signals && atomic_load(&probe_audio_frames));assert(!probe_audio_control(6,0,&value));printf("AUDIO native REV7 synthetic update/render/event OK signals=%u frames=%llu\n",signals,(unsigned long long)atomic_load(&probe_audio_frames));
 return 0;
}
