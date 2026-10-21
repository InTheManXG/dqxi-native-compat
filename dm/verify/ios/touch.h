// 参考 nxnative 的分组归属交互；摇杆独占，游戏键组内可滑动，系统键不串键。
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
typedef struct { float x, y, radius; int bit; const char *label; } DQControl;
typedef struct { uintptr_t id; int control; float x, y; } DQFinger;
typedef struct { DQControl controls[18]; DQFinger fingers[16]; float width,height,scale; } DQTouch;
static void dq_touch_layout(DQTouch *t, float w, float h) {
    // UIKit 可在尺寸不变时重复布局；不能因此释放玩家仍按住的触点。
    if(t->width==w && t->height==h)return;
    memset(t,0,sizeof *t);t->width=w;t->height=h;
    float s=fminf(1.3f,fminf(h/360.f,w/740.f));t->scale=s;
    t->controls[0]=(DQControl){85*s,h-170*s,54*s,-1,"移动"};
    t->controls[1]=(DQControl){w-205*s,h-76*s,47*s,-1,"镜头"};
    const char *labels[16]={"A","B","X","Y","L3","R3","L","R","ZL","ZR","+","−","◀","▲","▶","▼"};
    float fx=w-85*s,fy=h-170*s,dx=205*s,dy=h-76*s;
    const float x[16]={fx+40*s,fx,fx,fx-40*s,172*s,w-172*s,110*s,w-110*s,48*s,w-48*s,w/2+34*s,w/2-34*s,dx-29*s,dx,dx+29*s,dx};
    const float y[16]={fy,fy+40*s,fy-40*s,fy,40*s,40*s,40*s,40*s,40*s,40*s,32*s,32*s,dy,dy-29*s,dy,dy+29*s};
    for(int i=0;i<16;i++)t->controls[i+2]=(DQControl){x[i],y[i],(i>=12?15:22)*s,i,labels[i]};
}
static void dq_touch_clear(DQTouch *t) { memset(t->fingers,0,sizeof t->fingers); }
static int dq_touch_button_hit(const DQTouch *t,int c,float x,float y) {
    return hypotf(x-t->controls[c].x,y-t->controls[c].y)<=t->controls[c].radius+8*t->scale;
}
static int dq_touch_face(const DQTouch *t,float x,float y) {
    int best=-1;float distance=INFINITY;
    for(int c=2;c<6;c++){
        float d=hypotf(x-t->controls[c].x,y-t->controls[c].y);
        if(d<distance && dq_touch_button_hit(t,c,x,y)){best=c;distance=d;}
    }
    return best;
}
static void dq_touch_event(DQTouch *t,uintptr_t id,int phase,float x,float y) {
    if(!id)return;
    for(int i=0;i<16;i++)if(t->fingers[i].id==id){
        if(phase>=2)t->fingers[i]=(DQFinger){0};
        else {
            // 肩键和菜单键滑出后锁为无效，避免滑回或横穿时意外确认。
            int c=t->fingers[i].control;
            if(c>=6 && c<14 && !dq_touch_button_hit(t,c,x,y))t->fingers[i].control=-1;
            t->fingers[i].x=x;t->fingers[i].y=y;
        }
        return;
    }
    if(phase!=0)return;
    int owner=-1;
    for(int c=6;c<14;c++)if(dq_touch_button_hit(t,c,x,y)){owner=c;break;}
    if(owner<0){
        float best=1;
        // 用归一化距离选择组，扩大命中区而不扩大画面遮挡。
        const float cx[4]={t->controls[0].x,t->controls[1].x,t->controls[3].x,t->controls[15].x};
        const float cy[4]={t->controls[0].y,t->controls[1].y,t->controls[2].y,t->controls[14].y};
        const float radius[4]={t->controls[0].radius+10*t->scale,t->controls[1].radius+10*t->scale,72*t->scale,57*t->scale};
        const int groups[4]={0,1,2,14};
        for(int c=0;c<4;c++){float d=hypotf(x-cx[c],y-cy[c])/radius[c];if(d<=best){best=d;owner=groups[c];}}
    }
    if(owner<0)return;
    if(owner<2)for(int i=0;i<16;i++)if(t->fingers[i].id && t->fingers[i].control==owner)return;
    for(int i=0;i<16;i++)if(!t->fingers[i].id){t->fingers[i]=(DQFinger){id,owner,x,y};return;}
}
static void dq_touch_read(const DQTouch *t,uint64_t out[3]) {
    memset(out,0,3*sizeof *out);
    for(int i=0;i<16;i++)if(t->fingers[i].id){
        DQFinger f=t->fingers[i];if(f.control<0)continue;
        DQControl c=t->controls[f.control];
        if(f.control==2){int key=dq_touch_face(t,f.x,f.y);if(key>=0)out[0]|=1ull<<t->controls[key].bit;continue;}
        if(f.control==14){
            float x=f.x-t->controls[15].x,y=t->controls[14].y-f.y,d=hypotf(x,y);
            if(d<10*t->scale || d>70*t->scale)continue;
            // 每个扇区45度；斜向同时按两个方向，中心保持中立。
            int sector=((int)floorf(atan2f(y,x)/(3.14159265358979323846f/4)+.5f)+8)%8;
            const uint16_t bits[8]={1<<14,(1<<14)|(1<<13),1<<13,(1<<13)|(1<<12),1<<12,(1<<12)|(1<<15),1<<15,(1<<15)|(1<<14)};
            out[0]|=bits[sector];continue;
        }
        if(f.control>=6){if(dq_touch_button_hit(t,f.control,f.x,f.y))out[0]|=1ull<<c.bit;continue;}
        // 输入使用 UIKit 点而非视觉半径：iPad放大控件后，仍为6pt死区、60pt满幅。
        float x=f.x-c.x,y=c.y-f.y,len=hypotf(x,y);
        if(len<=6)continue;
        float scale=fminf((len-6)/54,1)/len;
        int32_t sx=lroundf(x*scale*32767),sy=lroundf(y*scale*32767);
        out[f.control+1]=(uint32_t)sx|((uint64_t)(uint32_t)sy<<32);
        int bit=16+f.control*4;
        if(sx< -16383)out[0]|=1ull<<bit;
        if(sy>16383)out[0]|=1ull<<(bit+1);
        if(sx>16383)out[0]|=1ull<<(bit+2);
        if(sy< -16383)out[0]|=1ull<<(bit+3);
    }
}
