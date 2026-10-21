#include "touch.h"
#include <assert.h>
#include <stdio.h>
int main(void){
    DQTouch t={0};uint64_t out[3];dq_touch_layout(&t,896,414);
    DQControl l=t.controls[0],r=t.controls[1],a=t.controls[2];
    dq_touch_event(&t,1,0,l.x,l.y);dq_touch_event(&t,1,1,l.x+100,l.y);
    dq_touch_event(&t,2,0,r.x,r.y);dq_touch_event(&t,2,1,r.x,r.y-100);
    dq_touch_event(&t,3,0,a.x,a.y);dq_touch_event(&t,4,0,a.x,a.y);
    dq_touch_read(&t,out);assert((out[0]&1) && (int32_t)out[1]==32767 && (int32_t)(out[2]>>32)==32767);
    dq_touch_event(&t,3,2,0,0);dq_touch_read(&t,out);assert(out[0]&1);
    dq_touch_event(&t,4,1,0,0);dq_touch_read(&t,out);assert(!(out[0]&1));
    dq_touch_event(&t,2,3,0,0);dq_touch_read(&t,out);assert(!out[2] && out[1]);
    dq_touch_event(&t,1,1,l.x+100,l.y-100);dq_touch_read(&t,out);
    assert(hypot((int32_t)out[1],(int32_t)(out[1]>>32))<32769);
    dq_touch_clear(&t);dq_touch_read(&t,out);assert(!(out[0]|out[1]|out[2]));
    dq_touch_event(&t,1,1,a.x,a.y);dq_touch_read(&t,out);assert(!out[0]);
    // 同组滑动切键，多指汇总不能在另一指抬起时误释放。
    dq_touch_event(&t,1,0,a.x,a.y);
    dq_touch_event(&t,1,1,t.controls[3].x,t.controls[3].y);
    dq_touch_read(&t,out);assert((out[0]&15)==2);
    dq_touch_event(&t,2,0,a.x,a.y);dq_touch_read(&t,out);assert((out[0]&15)==3);
    dq_touch_event(&t,1,2,0,0);dq_touch_read(&t,out);assert((out[0]&15)==1);
    dq_touch_layout(&t,896,414);dq_touch_read(&t,out);assert(out[0]&1);
    dq_touch_clear(&t);
    // 十字键同一触点连续走完八方向及中心死区，不必重新落指。
    float dx=t.controls[15].x,dy=t.controls[14].y;
    const uint16_t expected[8]={1<<14,(1<<14)|(1<<13),1<<13,(1<<13)|(1<<12),1<<12,(1<<12)|(1<<15),1<<15,(1<<15)|(1<<14)};
    dq_touch_event(&t,1,0,dx,dy);
    for(int i=0;i<8;i++){
        float angle=i*3.14159265358979323846f/4;
        dq_touch_event(&t,1,1,dx+30*cosf(angle),dy-30*sinf(angle));
        dq_touch_read(&t,out);assert(out[0]==expected[i]);
    }
    dq_touch_event(&t,1,1,dx,dy);dq_touch_read(&t,out);assert(!out[0]);
    dq_touch_event(&t,1,3,0,0);dq_touch_read(&t,out);assert(!out[0]);
    // 菜单键不得通过拖动串到确认键，也不得滑回重触发。
    DQControl plus=t.controls[12];dq_touch_event(&t,1,0,plus.x,plus.y);
    dq_touch_read(&t,out);assert(out[0]==(1<<10));
    dq_touch_event(&t,1,1,a.x,a.y);dq_touch_read(&t,out);assert(!out[0]);
    dq_touch_event(&t,1,1,plus.x,plus.y);dq_touch_read(&t,out);assert(!out[0]);dq_touch_clear(&t);
    // 三种画布等效行程；所有可见按钮必须留在安全画布内。
    const float sizes[3][2]={{740,360},{1194,810},{1366,1024}};
    for(int n=0;n<3;n++){
        dq_touch_layout(&t,sizes[n][0],sizes[n][1]);
        for(int i=0;i<18;i++){DQControl c=t.controls[i];assert(c.x>=c.radius && c.y>=c.radius && c.x+c.radius<=t.width && c.y+c.radius<=t.height);}
        l=t.controls[0];dq_touch_event(&t,1,0,l.x,l.y);
        dq_touch_event(&t,1,1,l.x+6,l.y);dq_touch_read(&t,out);assert(!out[1]);
        dq_touch_event(&t,1,1,l.x+33,l.y);dq_touch_read(&t,out);assert((int32_t)out[1]==16384);
        dq_touch_event(&t,1,1,l.x+60,l.y);dq_touch_read(&t,out);assert((int32_t)out[1]==32767);
        dq_touch_event(&t,2,0,l.x,l.y);dq_touch_event(&t,2,1,l.x-60,l.y);dq_touch_read(&t,out);assert((int32_t)out[1]==32767);
        dq_touch_clear(&t);
        a=t.controls[2];dq_touch_event(&t,1,0,a.x+a.radius+4*t.scale,a.y);dq_touch_read(&t,out);assert(out[0]&1);
        dq_touch_layout(&t,sizes[n][0]-1,sizes[n][1]);dq_touch_read(&t,out);assert(!(out[0]|out[1]|out[2]));
    }
    puts("touch: multitouch, group sliding, eight-way dpad, menu lockout, relayout and device-independent travel OK");
}
