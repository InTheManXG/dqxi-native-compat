// 合成红色 VP9 帧验证真实解码后的 YUV 平面，不含游戏内容。
#include <OMX_Component.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern OMX_ERRORTYPE probe_vp9_create(const OMX_CALLBACKTYPE*,void*,OMX_COMPONENTTYPE**);
extern OMX_ERRORTYPE probe_vp9_destroy(OMX_COMPONENTTYPE*);
static int frames,inputs,events,eos;
static unsigned width=320,height=240;
static OMX_ERRORTYPE ev(OMX_HANDLETYPE h,void*a,OMX_EVENTTYPE e,OMX_U32 x,OMX_U32 y,void*z){(void)h;(void)a;(void)x;(void)y;(void)z;assert(e!=OMX_EventError);events++;return 0;}
static OMX_ERRORTYPE empty_done(OMX_HANDLETYPE h,void*a,OMX_BUFFERHEADERTYPE*b){(void)h;(void)a;(void)b;inputs++;return 0;}
static OMX_ERRORTYPE full(OMX_HANDLETYPE h,void*a,OMX_BUFFERHEADERTYPE*b){
    (void)h;(void)a;
    if(!b->nFilledLen){if(b->nFlags&OMX_BUFFERFLAG_EOS)eos++;return 0;}
    unsigned stride=(width+1)&~1u,slice=(height+1)&~1u,uv=stride*slice;
    assert(b->nFilledLen==uv*3/2);
    // 检查可见区域的最后一行/列及色度尾部，防止奇数尺寸被截断或平面错位。
    assert(b->pBuffer[0]>=79&&b->pBuffer[0]<=83);
    assert(b->pBuffer[(height-1)*stride+width-1]>=79&&b->pBuffer[(height-1)*stride+width-1]<=83);
    assert(b->pBuffer[uv]>=88&&b->pBuffer[uv]<=92);
    assert(b->pBuffer[uv+1]>=238&&b->pBuffer[b->nFilledLen-1]>=238);
    frames++;return 0;
}
static void odd_frame(void) {
    OMX_CALLBACKTYPE cb={ev,empty_done,full};OMX_COMPONENTTYPE *c;
    assert(!probe_vp9_create(&cb,0,&c));width=321;height=241;
    OMX_PARAM_PORTDEFINITIONTYPE pd={.nSize=sizeof pd,.nPortIndex=0};
    assert(!c->GetParameter(c,OMX_IndexParamPortDefinition,&pd));
    pd.format.video.nFrameWidth=width;pd.format.video.nFrameHeight=height;
    assert(!c->SetParameter(c,OMX_IndexParamPortDefinition,&pd));pd.nPortIndex=1;
    assert(!c->GetParameter(c,OMX_IndexParamPortDefinition,&pd));
    assert(pd.format.video.nStride==322&&pd.format.video.nSliceHeight==242);
    assert(pd.nBufferSize==322*242*3/2);
    OMX_CONFIG_RECTTYPE crop={.nSize=sizeof crop,.nPortIndex=1};
    assert(!c->GetConfig(c,OMX_IndexConfigCommonOutputCrop,&crop));
    assert(crop.nWidth==width&&crop.nHeight==height);
    OMX_BUFFERHEADERTYPE *b[2][4];assert(!c->SendCommand(c,OMX_CommandStateSet,OMX_StateIdle,0));
    for(int p=0;p<2;p++)for(int i=0;i<4;i++)assert(!c->AllocateBuffer(c,&b[p][i],p,0,p?pd.nBufferSize:4*1024*1024));
    assert(!c->SendCommand(c,OMX_CommandStateSet,OMX_StateExecuting,0));
    assert(!c->FillThisBuffer(c,b[1][0]));
    // 自行生成的 321×241 纯红无损 VP9 帧，不包含游戏素材。
    static const unsigned char packet[]={0x82,0x49,0x83,0x42,0x0,0x14,0x0,0xf,0x6,0x0,0x38,0x24,0x1c,0x18,0x0,0x0,0x0,0x70,0x7d,0x8c,0x1a,0x7,0x5e,0xd0,0x0,0x78,0xaf,0x9c,0xe0,0xc,0x19,0xad,0xc4,0x9d,0x78,0x25,0x5b,0xd1,0x68,0xe4,0x5a,0x12,0x6f,0x72,0x92,0x9,0x18,0x45,0x81,0xba,0xc0,0x0};
    memcpy(b[0][0]->pBuffer,packet,sizeof packet);b[0][0]->nFilledLen=sizeof packet;
    int before=frames;assert(!c->EmptyThisBuffer(c,b[0][0]));assert(frames==before+1);
    assert(!c->SendCommand(c,OMX_CommandStateSet,OMX_StateIdle,0));
    assert(!c->SendCommand(c,OMX_CommandStateSet,OMX_StateLoaded,0));
    for(int p=0;p<2;p++)for(int i=0;i<4;i++)assert(!c->FreeBuffer(c,p,b[p][i]));
    assert(!probe_vp9_destroy(c));
}

int main(void){OMX_CALLBACKTYPE cb={ev,empty_done,full};OMX_COMPONENTTYPE *c;assert(!probe_vp9_create(&cb,0,&c));assert(c->SendCommand(c,OMX_CommandStateSet,OMX_StateExecuting,0)==OMX_ErrorIncorrectStateTransition);
OMX_PARAM_PORTDEFINITIONTYPE pd={.nSize=sizeof pd,.nPortIndex=0};
assert(!c->GetParameter(c,OMX_IndexParamPortDefinition,&pd));pd.nBufferSize=8192;
assert(!c->SetParameter(c,OMX_IndexParamPortDefinition,&pd));assert(!c->GetParameter(c,OMX_IndexParamPortDefinition,&pd));assert(pd.nBufferSize>=1024*1024);
pd.nSize=4;assert(c->SetParameter(c,OMX_IndexParamPortDefinition,&pd)==OMX_ErrorBadParameter);
OMX_BUFFERHEADERTYPE *b[2][4];assert(!c->SendCommand(c,OMX_CommandStateSet,OMX_StateIdle,0));for(int p=0;p<2;p++)for(int i=0;i<4;i++)assert(!c->AllocateBuffer(c,&b[p][i],p,0,p?320*240*3/2:4*1024*1024));assert(events==1);assert(!c->SendCommand(c,OMX_CommandStateSet,OMX_StateExecuting,0));assert(!c->FillThisBuffer(c,b[1][0]));static const unsigned char packet[] = {0x82,0x49,0x83,0x42,0x0,0x13,0xf0,0xe,0xf6,0x0,0x38,0x24,0x1c,0x18,0x4a,0x0,0x0,0x80,0x61,0xf6,0x30,0x68,0x1d,0x7b,0x40,0x0,0x12,0x3d,0x67,0x80,0x1,0x17,0x71,0x9a,0xa6,0x69,0xb6,0x2,0xcc,0xd2,0xd,0x64,0x98,0x80};
unsigned n=sizeof packet;memcpy(b[0][0]->pBuffer,packet,n);b[0][0]->nFilledLen=n;b[0][0]->nTimeStamp=123;assert(!c->EmptyThisBuffer(c,b[0][0]));assert(frames==1&&inputs==1);
assert(!c->FillThisBuffer(c,b[1][1]));assert(c->FillThisBuffer(c,b[1][1])==OMX_ErrorBadParameter);
assert(c->FreeBuffer(c,1,b[1][1])==OMX_ErrorIncorrectStateOperation);
b[0][0]->nFilledLen=0;b[0][0]->nFlags=OMX_BUFFERFLAG_EOS;
assert(!c->EmptyThisBuffer(c,b[0][0]));assert(eos==1&&frames==1&&inputs==2);
assert(!c->FillThisBuffer(c,b[1][2]));assert(!c->SendCommand(c,OMX_CommandStateSet,OMX_StateIdle,0));
assert(!c->SendCommand(c,OMX_CommandStateSet,OMX_StateLoaded,0));
for(int p=0;p<2;p++)for(int i=0;i<4;i++)assert(!c->FreeBuffer(c,p,b[p][i]));assert(!probe_vp9_destroy(c));odd_frame();puts("VP9 even/odd synthetic frames decoded with valid NV12 planes and crop");}
