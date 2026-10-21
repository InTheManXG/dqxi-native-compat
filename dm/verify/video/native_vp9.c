// 探针的原生 VP9/OpenMAX 桥：游戏负责解复用、时钟和纹理上传，libvpx 只解码真实视频包。
// Native VP9/OpenMAX bridge: guest demux, clocks and texture upload; libvpx decodes real packets.
// shortcut: 仅实现启动路径所需的 8 位 NV12、最多 32 个缓冲；扩展格式前需新增真实视频回归。
#include <OMX_Component.h>
#include <vpx/vpx_decoder.h>
#include <vpx/vp8dx.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#define VP9_CODING ((OMX_VIDEO_CODINGTYPE)10)
#define TRACE(...) do { dprintf(2, "PROBE VP9 " __VA_ARGS__); dprintf(2, "\n"); } while (0)
#define VERSION ((OMX_VERSIONTYPE){.nVersion=0x10101})
typedef struct {
    OMX_COMPONENTTYPE api;
    OMX_CALLBACKTYPE cb;
    OMX_PTR app;
    OMX_STATETYPE state, target;
    OMX_PARAM_PORTDEFINITIONTYPE port[2];
    OMX_BUFFERHEADERTYPE *buffers[2][32], *input[32], *output[32];
    unsigned allocated[2], ni, no;
    int pending_enable[2], pending_disable[2], pumping, decoded, eos;
    OMX_TICKS pts;
    vpx_codec_ctx_t decoder;
    vpx_codec_iter_t iter;
    vpx_image_t *frame;
} decoder_t;
static decoder_t *self(OMX_HANDLETYPE h) { return (decoder_t *)h; }
static void output_dimensions(decoder_t *d,unsigned width,unsigned height) {
    OMX_VIDEO_PORTDEFINITIONTYPE *v=&d->port[1].format.video;
    v->nFrameWidth=width;v->nFrameHeight=height;
    // NV12 色度以 2×2 像素采样；存储补齐偶数，裁切尺寸仍保留真实宽高。
    v->nStride=(width+1)&~1u;v->nSliceHeight=(height+1)&~1u;
    d->port[1].nBufferSize=v->nStride*v->nSliceHeight*3/2;
}

static void event(decoder_t *d, OMX_EVENTTYPE e, OMX_U32 a, OMX_U32 b) { d->cb.EventHandler(&d->api,d->app,e,a,b,NULL); }
static void transitions(decoder_t *d) {
    for (int p=0;p<2;p++) {
        d->port[p].bPopulated=d->allocated[p]>=d->port[p].nBufferCountActual;
        if(d->pending_disable[p]&&!d->allocated[p]) {d->pending_disable[p]=0;event(d,OMX_EventCmdComplete,OMX_CommandPortDisable,p);}
        if(d->pending_enable[p]&&d->port[p].bPopulated) {d->pending_enable[p]=0;event(d,OMX_EventCmdComplete,OMX_CommandPortEnable,p);}
    }
    if(d->target==d->state)return;
    if(d->target==OMX_StateIdle&&d->state==OMX_StateLoaded)
        for(int p=0;p<2;p++)if(d->port[p].bEnabled&&!d->port[p].bPopulated)return;
    if(d->target==OMX_StateLoaded&&(d->allocated[0]||d->allocated[1]))return;
    d->state=d->target;event(d,OMX_EventCmdComplete,OMX_CommandStateSet,d->state);
}
static OMX_BUFFERHEADERTYPE *pop(OMX_BUFFERHEADERTYPE **q,unsigned *n) {OMX_BUFFERHEADERTYPE *b=q[0];memmove(q,q+1,(--*n)*sizeof *q);return b;}
static void pump(decoder_t *d) {
    if(d->pumping)return;d->pumping=1;
    while(d->state==OMX_StateExecuting&&d->port[1].bEnabled&&d->no) {
        if(!d->frame&&!d->decoded) {
            if(!d->ni)break;
            OMX_BUFFERHEADERTYPE *in=pop(d->input,&d->ni);
            d->pts=in->nTimeStamp;d->eos=!!(in->nFlags&OMX_BUFFERFLAG_EOS);
            if((in->nFilledLen || d->eos) && vpx_codec_decode(&d->decoder,in->nFilledLen ? in->pBuffer+in->nOffset : NULL,in->nFilledLen,NULL,0)) {
                TRACE("decode error: %s bytes=%u flags=%#x",vpx_codec_error(&d->decoder),in->nFilledLen,in->nFlags);event(d,OMX_EventError,OMX_ErrorStreamCorrupt,0);
                d->cb.EmptyBufferDone(&d->api,d->app,in);break;
            }
            d->iter=NULL;d->decoded=1;d->cb.EmptyBufferDone(&d->api,d->app,in);
        }
        if(!d->frame)d->frame=vpx_codec_get_frame(&d->decoder,&d->iter);
        if(!d->frame) {
            d->decoded=0;
            if(d->eos) {OMX_BUFFERHEADERTYPE *out=pop(d->output,&d->no);out->nFilledLen=0;out->nOffset=0;out->nFlags=OMX_BUFFERFLAG_EOS;out->nTimeStamp=d->pts;d->eos=0;d->cb.FillBufferDone(&d->api,d->app,out);}
            continue;
        }
        vpx_image_t *im=d->frame;
        if(im->fmt!=VPX_IMG_FMT_I420) {event(d,OMX_EventError,OMX_ErrorUnsupportedSetting,0);break;}
        OMX_VIDEO_PORTDEFINITIONTYPE *v=&d->port[1].format.video;
        if(v->nFrameWidth!=im->d_w||v->nFrameHeight!=im->d_h) {
            if(!im->d_w||!im->d_h||im->d_w>8192||im->d_h>8192) {event(d,OMX_EventError,OMX_ErrorUnsupportedSetting,0);break;}
            output_dimensions(d,im->d_w,im->d_h);
            TRACE("port dimensions %ux%u",im->d_w,im->d_h);event(d,OMX_EventPortSettingsChanged,1,0);break;
        }
        OMX_BUFFERHEADERTYPE *out=pop(d->output,&d->no);
        if(out->nAllocLen<d->port[1].nBufferSize) {event(d,OMX_EventError,OMX_ErrorOverflow,0);break;}
        // SDK 视频出口消费 NV12；必须交织 U/V，不能把 I420 的两个色度平面直接拼接。
        // The SDK video output consumes NV12: interleave U/V rather than concatenate I420 planes.
        unsigned stride=v->nStride,slice=v->nSliceHeight,offset=stride*slice;
        for(unsigned row=0;row<slice;row++) {
            unsigned source_row=row<im->d_h?row:im->d_h-1;
            memcpy(out->pBuffer+row*stride,im->planes[0]+source_row*im->stride[0],im->d_w);
            if(stride>im->d_w)out->pBuffer[row*stride+im->d_w]=out->pBuffer[row*stride+im->d_w-1];
        }
        for(unsigned row=0;row<slice/2;row++) for(unsigned col=0;col<stride/2;col++) {
            out->pBuffer[offset++]=im->planes[1][row*im->stride[1]+col];
            out->pBuffer[offset++]=im->planes[2][row*im->stride[2]+col];
        }
        out->nOffset=0;out->nFilledLen=offset;out->nTimeStamp=d->pts;out->nFlags=OMX_BUFFERFLAG_ENDOFFRAME;
        d->frame=NULL;TRACE("decoded frame pts=%lld bytes=%u",(long long)d->pts,offset);d->cb.FillBufferDone(&d->api,d->app,out);
    }
    d->pumping=0;
}
static OMX_ERRORTYPE getparam(OMX_HANDLETYPE h,OMX_INDEXTYPE ix,OMX_PTR param) {
    if (!param || *(OMX_U32 *)param < 12) return OMX_ErrorBadParameter;
    decoder_t *d=self(h);OMX_U32 *w=param;TRACE("GetParameter %#x size=%u port=%u",ix,w[0],w[2]);
    if(ix==OMX_IndexParamPortDefinition) {OMX_PARAM_PORTDEFINITIONTYPE *p=param;if(p->nSize!=sizeof *p||p->nPortIndex>1)return OMX_ErrorBadParameter;*p=d->port[p->nPortIndex];return OMX_ErrorNone;}
    if(ix==OMX_IndexParamVideoPortFormat) {OMX_VIDEO_PARAM_PORTFORMATTYPE *p=param;if(p->nSize!=sizeof *p||p->nPortIndex>1)return OMX_ErrorBadParameter;if(p->nIndex)return OMX_ErrorNoMore;p->eCompressionFormat=p->nPortIndex?OMX_VIDEO_CodingUnused:VP9_CODING;p->eColorFormat=p->nPortIndex?OMX_COLOR_FormatYUV420SemiPlanar:OMX_COLOR_FormatUnused;p->xFramerate=30<<16;return OMX_ErrorNone;}
    if(ix==OMX_IndexParamVideoInit) {OMX_PORT_PARAM_TYPE *p=param;if(p->nSize!=sizeof *p)return OMX_ErrorBadParameter;p->nPorts=2;p->nStartPortNumber=0;return OMX_ErrorNone;}
    if(ix==OMX_IndexParamStandardComponentRole) {OMX_PARAM_COMPONENTROLETYPE *p=param;if(p->nSize!=sizeof *p)return OMX_ErrorBadParameter;strcpy((char*)p->cRole,"video_decoder.vp9");return OMX_ErrorNone;}
    return OMX_ErrorUnsupportedIndex;
}
static OMX_ERRORTYPE setparam(OMX_HANDLETYPE h,OMX_INDEXTYPE ix,OMX_PTR param) {
    if (!param || *(OMX_U32 *)param < 12) return OMX_ErrorBadParameter;
    decoder_t *d=self(h);OMX_U32 *w=param;TRACE("SetParameter %#x size=%u port=%u",ix,w[0],w[2]);
    if(ix==OMX_IndexParamPortDefinition) {OMX_PARAM_PORTDEFINITIONTYPE *p=param;if(p->nSize!=sizeof *p||p->nPortIndex>1||p->nBufferCountActual<4||p->nBufferCountActual>32||!p->nBufferSize||p->nBufferSize>64*1024*1024||!p->format.video.nFrameWidth||!p->format.video.nFrameHeight||p->format.video.nFrameWidth>8192||p->format.video.nFrameHeight>8192)return OMX_ErrorBadParameter;
        d->port[p->nPortIndex].nBufferCountActual=p->nBufferCountActual;d->port[p->nPortIndex].nBufferSize=p->nBufferSize;
        // 保留完整压缩帧容量；不能接受播放器的小建议值后截断大型关键帧。
        // Preserve capacity for whole compressed keyframes rather than a small client hint.
        if(!p->nPortIndex && d->port[0].nBufferSize<4*1024*1024)d->port[0].nBufferSize=4*1024*1024;
        if(p->nPortIndex==0) {d->port[0].format.video=p->format.video;output_dimensions(d,p->format.video.nFrameWidth,p->format.video.nFrameHeight);}
        else {unsigned required=d->port[1].format.video.nStride*d->port[1].format.video.nSliceHeight*3/2;if(d->port[1].nBufferSize<required)d->port[1].nBufferSize=required;}
        return OMX_ErrorNone;}
    if(ix==OMX_IndexParamVideoPortFormat) {OMX_VIDEO_PARAM_PORTFORMATTYPE *p=param;if(p->nSize!=sizeof *p||p->nPortIndex>1||p->nIndex)return OMX_ErrorBadParameter;if(p->nPortIndex&&p->eColorFormat!=OMX_COLOR_FormatYUV420SemiPlanar)return OMX_ErrorUnsupportedSetting;return OMX_ErrorNone;}
    if(ix==OMX_IndexParamStandardComponentRole) {
        OMX_PARAM_COMPONENTROLETYPE *p=param;
        if(p->nSize!=sizeof *p||!memchr(p->cRole,0,sizeof p->cRole))return OMX_ErrorBadParameter;
        return strcmp((char*)p->cRole,"video_decoder.vp9")?OMX_ErrorUnsupportedSetting:OMX_ErrorNone;
    }
    return OMX_ErrorUnsupportedIndex;
}
static OMX_ERRORTYPE getconfig(OMX_HANDLETYPE h,OMX_INDEXTYPE ix,OMX_PTR param) {
    decoder_t *d=self(h);TRACE("GetConfig %#x",ix);
    if(ix==OMX_IndexConfigCommonOutputCrop) {OMX_CONFIG_RECTTYPE *p=param;if(!p||p->nSize!=sizeof *p)return OMX_ErrorBadParameter;if(p->nPortIndex!=1)return OMX_ErrorBadPortIndex;p->nLeft=p->nTop=0;p->nWidth=d->port[1].format.video.nFrameWidth;p->nHeight=d->port[1].format.video.nFrameHeight;return OMX_ErrorNone;}
    return OMX_ErrorUnsupportedIndex;
}
static OMX_ERRORTYPE setconfig(OMX_HANDLETYPE h,OMX_INDEXTYPE ix,OMX_PTR param){(void)h;(void)param;TRACE("SetConfig %#x",ix);return OMX_ErrorUnsupportedIndex;}
static OMX_ERRORTYPE extension(OMX_HANDLETYPE h,OMX_STRING name,OMX_INDEXTYPE *index){(void)h;(void)index;TRACE("Extension %s",name);return OMX_ErrorUnsupportedIndex;}
static OMX_ERRORTYPE getstate(OMX_HANDLETYPE h,OMX_STATETYPE *s){*s=self(h)->state;return OMX_ErrorNone;}
static OMX_ERRORTYPE command(OMX_HANDLETYPE h,OMX_COMMANDTYPE c,OMX_U32 port,OMX_PTR data) {
    decoder_t *d=self(h);(void)data;TRACE("Command %u arg=%u",c,port);
    if(c==OMX_CommandStateSet) {
        if(port==d->state)return OMX_ErrorSameState;
        if(!((d->state==OMX_StateLoaded&&port==OMX_StateIdle) ||
             (d->state==OMX_StateIdle&&(port==OMX_StateLoaded||port==OMX_StateExecuting)) ||
             (d->state==OMX_StateExecuting&&(port==OMX_StateIdle||port==OMX_StatePause)) ||
             (d->state==OMX_StatePause&&(port==OMX_StateExecuting||port==OMX_StateIdle))))return OMX_ErrorIncorrectStateTransition;
        d->target=port;
        // 停止必须归还所有排队缓冲，不能让客户端释放仍被组件持有的指针。
        // Return queued buffers before clients free them during a stop transition.
        if(port==OMX_StateIdle&&d->state!=OMX_StateLoaded) {
            d->state=OMX_StateIdle;d->frame=NULL;d->decoded=0;d->eos=0;
            while(d->ni){OMX_BUFFERHEADERTYPE *b=pop(d->input,&d->ni);b->nFilledLen=0;d->cb.EmptyBufferDone(h,d->app,b);}
            while(d->no){OMX_BUFFERHEADERTYPE *b=pop(d->output,&d->no);b->nFilledLen=0;b->nFlags=0;d->cb.FillBufferDone(h,d->app,b);}
            event(d,OMX_EventCmdComplete,c,port);
        } else transitions(d);
        pump(d);return OMX_ErrorNone;
    }
    if(c==OMX_CommandPortDisable||c==OMX_CommandPortEnable||c==OMX_CommandFlush) {
        if(port!=OMX_ALL&&port>1)return OMX_ErrorBadPortIndex;
        for(unsigned p=0;p<2;p++)if(port==OMX_ALL||port==p) {
            if(c!=OMX_CommandPortEnable) {while(p?d->no:d->ni){OMX_BUFFERHEADERTYPE *b=p?pop(d->output,&d->no):pop(d->input,&d->ni);b->nFilledLen=0;if(p)d->cb.FillBufferDone(h,d->app,b);else d->cb.EmptyBufferDone(h,d->app,b);}}
            if(c==OMX_CommandPortDisable){d->port[p].bEnabled=0;d->pending_disable[p]=1;}
            if(c==OMX_CommandPortEnable){d->port[p].bEnabled=1;d->pending_enable[p]=1;}
            if(c==OMX_CommandFlush){d->frame=NULL;d->decoded=0;event(d,OMX_EventCmdComplete,c,p);}
        }
        transitions(d);pump(d);return OMX_ErrorNone;
    }
    return OMX_ErrorNotImplemented;
}
static OMX_ERRORTYPE usebuffer(OMX_HANDLETYPE h,OMX_BUFFERHEADERTYPE **out,OMX_U32 port,OMX_PTR app,OMX_U32 bytes,OMX_U8 *memory) {
    decoder_t *d=self(h);TRACE("UseBuffer port=%u size=%u",port,bytes);
    if(!out||!memory||port>1||d->allocated[port]>=32||bytes<d->port[port].nBufferSize||bytes>64*1024*1024)return OMX_ErrorBadParameter;
    OMX_BUFFERHEADERTYPE *b=calloc(1,sizeof *b);if(!b)return OMX_ErrorInsufficientResources;
    b->nSize=sizeof *b;b->nVersion=VERSION;b->pBuffer=memory;b->nAllocLen=bytes;b->pAppPrivate=app;b->nInputPortIndex=port?OMX_ALL:0;b->nOutputPortIndex=port?1:OMX_ALL;
    d->buffers[port][d->allocated[port]++]=b;*out=b;transitions(d);return OMX_ErrorNone;
}
static OMX_ERRORTYPE allocate(OMX_HANDLETYPE h,OMX_BUFFERHEADERTYPE **out,OMX_U32 p,OMX_PTR app,OMX_U32 n){if(n>64*1024*1024)return OMX_ErrorBadParameter;void *mem=malloc(n);if(!mem)return OMX_ErrorInsufficientResources;OMX_ERRORTYPE r=usebuffer(h,out,p,app,n,mem);if(r)free(mem);else(*out)->pPlatformPrivate=mem;return r;}
static int queued(OMX_BUFFERHEADERTYPE **queue,unsigned count,OMX_BUFFERHEADERTYPE *buffer) {
    for(unsigned i=0;i<count;i++)if(queue[i]==buffer)return 1;
    return 0;
}
static int registered(decoder_t *d,unsigned port,OMX_BUFFERHEADERTYPE *buffer) {
    return port<2&&queued(d->buffers[port],d->allocated[port],buffer);
}
static OMX_ERRORTYPE release(OMX_HANDLETYPE h,OMX_U32 p,OMX_BUFFERHEADERTYPE *b) {
    decoder_t *d=self(h);
    if(!registered(d,p,b))return OMX_ErrorBadParameter;
    if(queued(d->input,d->ni,b)||queued(d->output,d->no,b))return OMX_ErrorIncorrectStateOperation;
    unsigned i=0;while(d->buffers[p][i]!=b)i++;
    d->buffers[p][i]=d->buffers[p][--d->allocated[p]];free(b->pPlatformPrivate);free(b);transitions(d);return OMX_ErrorNone;
}
static OMX_ERRORTYPE empty(OMX_HANDLETYPE h,OMX_BUFFERHEADERTYPE *b) {
    decoder_t *d=self(h);
    if(!registered(d,0,b)||d->ni==32||queued(d->input,d->ni,b)||b->nOffset>b->nAllocLen||b->nFilledLen>b->nAllocLen-b->nOffset)return OMX_ErrorBadParameter;
    d->input[d->ni++]=b;pump(d);return OMX_ErrorNone;
}
static OMX_ERRORTYPE fill(OMX_HANDLETYPE h,OMX_BUFFERHEADERTYPE *b) {
    decoder_t *d=self(h);
    if(!registered(d,1,b)||d->no==32||queued(d->output,d->no,b))return OMX_ErrorBadParameter;
    d->output[d->no++]=b;pump(d);return OMX_ErrorNone;
}
static OMX_ERRORTYPE callbacks(OMX_HANDLETYPE h,OMX_CALLBACKTYPE *cb,OMX_PTR app){self(h)->cb=*cb;self(h)->app=app;return OMX_ErrorNone;}
static OMX_ERRORTYPE deinit(OMX_HANDLETYPE h){decoder_t *d=self(h);if(d->allocated[0]||d->allocated[1])return OMX_ErrorIncorrectStateOperation;vpx_codec_destroy(&d->decoder);return OMX_ErrorNone;}
static OMX_ERRORTYPE role(OMX_HANDLETYPE h,OMX_U8 *name,OMX_U32 index){(void)h;if(index)return OMX_ErrorNoMore;strcpy((char*)name,"video_decoder.vp9");return OMX_ErrorNone;}
static OMX_ERRORTYPE version(OMX_HANDLETYPE h,OMX_STRING name,OMX_VERSIONTYPE *v,OMX_VERSIONTYPE *spec,OMX_UUIDTYPE *uuid){(void)h;strcpy(name,"OMX.google.vp9.decoder");*v=*spec=VERSION;memset(uuid,0,sizeof *uuid);return OMX_ErrorNone;}
OMX_ERRORTYPE probe_vp9_create(const OMX_CALLBACKTYPE *cb,void *app,OMX_COMPONENTTYPE **out){
    if(!cb||!cb->EventHandler||!cb->EmptyBufferDone||!cb->FillBufferDone||!out)return OMX_ErrorBadParameter;
    decoder_t *d=calloc(1,sizeof *d);if(!d)return OMX_ErrorInsufficientResources;
    vpx_codec_dec_cfg_t cfg={.threads=4};if(vpx_codec_dec_init(&d->decoder,vpx_codec_vp9_dx(),&cfg,0)){free(d);return OMX_ErrorInsufficientResources;}
    d->state=d->target=OMX_StateLoaded;d->cb=*cb;d->app=app;
    for(int p=0;p<2;p++){d->port[p]=(OMX_PARAM_PORTDEFINITIONTYPE){.nSize=sizeof d->port[p],.nVersion=VERSION,.nPortIndex=p,.eDir=p?OMX_DirOutput:OMX_DirInput,.nBufferCountActual=4,.nBufferCountMin=4,.nBufferSize=p?320*240*3/2:4*1024*1024,.bEnabled=1,.eDomain=OMX_PortDomainVideo,.nBufferAlignment=16};d->port[p].format.video=(OMX_VIDEO_PORTDEFINITIONTYPE){.nFrameWidth=320,.nFrameHeight=240,.nStride=320,.nSliceHeight=240,.xFramerate=30<<16,.eCompressionFormat=p?OMX_VIDEO_CodingUnused:VP9_CODING,.eColorFormat=p?OMX_COLOR_FormatYUV420SemiPlanar:OMX_COLOR_FormatUnused};}
    d->api=(OMX_COMPONENTTYPE){.nSize=sizeof d->api,.nVersion=VERSION,.pComponentPrivate=d,.pApplicationPrivate=app,.GetComponentVersion=version,.SendCommand=command,.GetParameter=getparam,.SetParameter=setparam,.GetConfig=getconfig,.SetConfig=setconfig,.GetExtensionIndex=extension,.GetState=getstate,.UseBuffer=usebuffer,.AllocateBuffer=allocate,.FreeBuffer=release,.EmptyThisBuffer=empty,.FillThisBuffer=fill,.SetCallbacks=callbacks,.ComponentDeInit=deinit,.ComponentRoleEnum=role};
    *out=&d->api;TRACE("created component portdef=%zu header=%zu",sizeof d->port[0],sizeof(OMX_BUFFERHEADERTYPE));return OMX_ErrorNone;
}
OMX_ERRORTYPE probe_vp9_destroy(OMX_COMPONENTTYPE *c){OMX_ERRORTYPE e=deinit(c);if(!e)free(c);return e;}
