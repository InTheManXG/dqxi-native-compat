// 独立 UIKit 外壳：输入走原 Pro 手柄协议，资源和存档只访问本应用沙箱。
#import <UIKit/UIKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <AVFAudio/AVFAudio.h>
#include <pthread.h>
#include <sys/resource.h>
#include "../probe_input.h"
#include "touch.h"
uint64_t probe_keyboard_buttons,g_t0_ns;
int probe_keyboard_ready,probe_keyboard_modal;
extern uint64_t now_ns(void);
extern int module_probe_main(int,char **);
extern int probe_memory_prepare(void);
static CAMetalLayer *metal;
static pthread_mutex_t input_lock=PTHREAD_MUTEX_INITIALIZER;
static uint64_t input[3];
static UIViewController *root;
void *front_metal_layer(void){return (__bridge void *)metal;}
void probe_controller_read(uint64_t out[3]){pthread_mutex_lock(&input_lock);memcpy(out,input,sizeof input);pthread_mutex_unlock(&input_lock);}
@interface DQPad : UIView { DQTouch touch; }
-(void)clear;
@end
@implementation DQPad
-(instancetype)initWithFrame:(CGRect)frame {if((self=[super initWithFrame:frame])){self.multipleTouchEnabled=YES;self.opaque=NO;self.backgroundColor=UIColor.clearColor;}return self;}
-(void)publish {uint64_t state[3];dq_touch_read(&touch,state);pthread_mutex_lock(&input_lock);memcpy(input,state,sizeof input);pthread_mutex_unlock(&input_lock);[self setNeedsDisplay];}
-(void)layoutSubviews {[super layoutSubviews];dq_touch_layout(&touch,self.bounds.size.width,self.bounds.size.height);[self publish];}
-(void)clear {dq_touch_clear(&touch);[self publish];}
-(void)update:(NSSet<UITouch *> *)touches phase:(int)phase {
    if(__atomic_load_n(&probe_keyboard_modal,__ATOMIC_ACQUIRE)){[self clear];return;}
    for(UITouch *t in touches){CGPoint p=[t locationInView:self];dq_touch_event(&touch,(uintptr_t)(__bridge void *)t,phase,p.x,p.y);}
    [self publish];
}
-(void)touchesBegan:(NSSet *)t withEvent:(UIEvent *)e{[self update:t phase:0];}
-(void)touchesMoved:(NSSet *)t withEvent:(UIEvent *)e{[self update:t phase:1];}
-(void)touchesEnded:(NSSet *)t withEvent:(UIEvent *)e{[self update:t phase:2];}
-(void)touchesCancelled:(NSSet *)t withEvent:(UIEvent *)e{[self update:t phase:3];}
-(void)drawRect:(CGRect)rect {
    uint64_t state[3];dq_touch_read(&touch,state);
    // 十字键显示为连续区域，与单指滑动、斜向输入的实际命中语义一致。
    CGFloat s=touch.scale,dx=touch.controls[15].x,dy=touch.controls[14].y;
    [[UIColor colorWithWhite:.05 alpha:.3] setFill];
    [[UIBezierPath bezierPathWithRoundedRect:CGRectMake(dx-44*s,dy-15*s,88*s,30*s) cornerRadius:6*s] fill];
    [[UIBezierPath bezierPathWithRoundedRect:CGRectMake(dx-15*s,dy-44*s,30*s,88*s) cornerRadius:6*s] fill];
    for(int i=0;i<18;i++){DQControl c=touch.controls[i];CGRect box=CGRectMake(c.x-c.radius,c.y-c.radius,c.radius*2,c.radius*2);
        BOOL held=i>=2 && (state[0]&(1ull<<c.bit));
        [(held ? [UIColor colorWithWhite:1 alpha:.42] : [UIColor colorWithWhite:.05 alpha:.3]) setFill];
        UIBezierPath *circle=i>=14 ? [UIBezierPath bezierPathWithRoundedRect:box cornerRadius:5*s] : [UIBezierPath bezierPathWithOvalInRect:box];[circle fill];[[UIColor colorWithWhite:1 alpha:.65] setStroke];circle.lineWidth=1.5;[circle stroke];
        NSString *label=@(c.label);NSDictionary *attrs=@{NSFontAttributeName:[UIFont boldSystemFontOfSize:15],NSForegroundColorAttributeName:UIColor.whiteColor};CGSize size=[label sizeWithAttributes:attrs];
        [label drawAtPoint:CGPointMake(c.x-size.width/2,c.y-size.height/2) withAttributes:attrs];
        if(i<2){float x=(int32_t)state[i+1]/32767.f,y=(int32_t)(state[i+1]>>32)/32767.f;[[UIColor colorWithWhite:1 alpha:.45] setFill];[[UIBezierPath bezierPathWithOvalInRect:CGRectMake(c.x+x*c.radius*.65-13,c.y-y*c.radius*.65-13,26,26)] fill];}
    }
}
@end
static DQPad *pad;
// 游戏线程等待 UIKit 回答；不能阻塞主线程，也不能在主线程执行客体校验回调。
NSString *probe_ios_prompt(NSString *title,NSString *message,NSString *initial,BOOL secure,BOOL confirm) {
    dispatch_semaphore_t done=dispatch_semaphore_create(0);__block NSString *result=nil;
    dispatch_async(dispatch_get_main_queue(), ^{
        [pad clear];UIAlertController *a=[UIAlertController alertControllerWithTitle:title message:message preferredStyle:UIAlertControllerStyleAlert];
        if(!confirm)[a addTextFieldWithConfigurationHandler:^(UITextField *f){f.text=initial;f.secureTextEntry=secure;f.autocorrectionType=UITextAutocorrectionTypeNo;}];
        [a addAction:[UIAlertAction actionWithTitle:confirm ? @"继续使用" : @"确定" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action){result=confirm ? @"" : a.textFields.firstObject.text;dispatch_semaphore_signal(done);}]];
        [a addAction:[UIAlertAction actionWithTitle:confirm ? @"重新输入" : @"取消" style:UIAlertActionStyleCancel handler:^(UIAlertAction *action){dispatch_semaphore_signal(done);}]];
        [root presentViewController:a animated:YES completion:nil];
    });dispatch_semaphore_wait(done,DISPATCH_TIME_FOREVER);return result;
}
static void envpath(const char *name,NSString *path){setenv(name,path.fileSystemRepresentation,1);}
static void *game_thread(void *unused){@autoreleasepool{
    NSString *modules=[NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"modules"];
    char *args[]={"DQXI",(char *)modules.fileSystemRepresentation,"--start","_ZN2nn4init5StartEmmPFvvES2_","main","subsdk0","sdk",NULL};
    g_t0_ns=now_ns();int result=module_probe_main(7,args);fprintf(stderr,"game returned %d\n",result);
    dispatch_async(dispatch_get_main_queue(), ^{[pad clear];UIAlertController *alert=[UIAlertController alertControllerWithTitle:@"游戏已停止" message:@"请导出 boot.txt 检查原因。" preferredStyle:UIAlertControllerStyleAlert];[alert addAction:[UIAlertAction actionWithTitle:@"确定" style:0 handler:nil]];[root presentViewController:alert animated:YES completion:nil];});
}return NULL;}
@interface DQController : UIViewController
@property UILabel *notice;
@property UIButton *start;
@property BOOL started;
@end
@implementation DQController
-(BOOL)prefersStatusBarHidden{return YES;}
-(UIInterfaceOrientationMask)supportedInterfaceOrientations{return UIInterfaceOrientationMaskLandscape;}
-(void)viewDidLoad{[super viewDidLoad];self.view.backgroundColor=UIColor.blackColor;
    metal=[CAMetalLayer layer];metal.device=MTLCreateSystemDefaultDevice();metal.pixelFormat=MTLPixelFormatBGRA8Unorm;metal.drawableSize=CGSizeMake(1920,1080);[self.view.layer addSublayer:metal];
    pad=[[DQPad alloc]initWithFrame:self.view.bounds];pad.hidden=YES;[self.view addSubview:pad];
    self.notice=[UILabel new];self.notice.numberOfLines=0;self.notice.textColor=UIColor.whiteColor;self.notice.textAlignment=NSTextAlignmentCenter;self.notice.text=@"DQXI S · iPhone 测试版\n在“文件 → 我的 iPhone → DQXI S”中放入 romfs-storage.bin，再点启动。\n双摇杆可同时操作；A 确认、B 返回，方向键选择菜单。";[self.view addSubview:self.notice];
    self.start=[UIButton buttonWithType:UIButtonTypeSystem];[self.start setTitle:@"检查资源并启动" forState:UIControlStateNormal];[self.start addTarget:self action:@selector(startGame) forControlEvents:UIControlEventTouchUpInside];[self.view addSubview:self.start];
}
-(void)viewDidLayoutSubviews{[super viewDidLayoutSubviews];CGRect b=self.view.bounds;CGFloat w=fmin(b.size.width,b.size.height*16/9),h=w*9/16;
    [CATransaction begin];[CATransaction setDisableActions:YES];metal.frame=CGRectMake((b.size.width-w)/2,(b.size.height-h)/2,w,h);[CATransaction commit];
    pad.frame=UIEdgeInsetsInsetRect(b,self.view.safeAreaInsets);self.notice.frame=CGRectMake(50,30,b.size.width-100,b.size.height-130);self.start.frame=CGRectMake(100,b.size.height-100,b.size.width-200,55);
}
-(void)startGame {
    if(self.started)return;
    NSString *docs=NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
    NSString *romfs=[docs stringByAppendingPathComponent:@"romfs-storage.bin"];
    NSNumber *size=[[NSFileManager.defaultManager attributesOfItemAtPath:romfs error:nil] objectForKey:NSFileSize];
    NSNumber *expected=[NSBundle.mainBundle objectForInfoDictionaryKey:@"DQRomFSBytes"];
    if(!size || size.unsignedLongLongValue!=expected.unsignedLongLongValue){self.notice.text=[NSString stringWithFormat:@"资源未就绪\n需要 romfs-storage.bin：%@ 字节\n当前：%@ 字节\n请完成复制后重试。",expected,size ?: @0];return;}
    if(!metal.device.supportsBCTextureCompression){self.notice.text=@"当前设备不支持本包需要的 BC 纹理格式。";return;}
    struct rlimit files={4096,4096};setrlimit(RLIMIT_NOFILE,&files);
    NSString *log=[docs stringByAppendingPathComponent:@"boot.txt"];freopen(log.fileSystemRepresentation,"a",stderr);setvbuf(stderr,NULL,_IOLBF,0);
    NSString *bundle=NSBundle.mainBundle.resourcePath,*fw=NSBundle.mainBundle.privateFrameworksPath;
    envpath("DM_PROBE_ROMFS",romfs);envpath("DM_PROBE_SAVE_DIR",[docs stringByAppendingPathComponent:@"saves"]);
    envpath("DM_NVN_SHMAP",[bundle stringByAppendingPathComponent:@"shader-map.tsv"]);envpath("DM_MSL_DIR",[bundle stringByAppendingPathComponent:@"shader-cache"]);
    envpath("DM_PROBE_AUDIO_LIB",[fw stringByAppendingPathComponent:@"AudioBridge.framework/AudioBridge"]);envpath("DM_PROBE_VIDEO_LIB",[fw stringByAppendingPathComponent:@"NativeVP9.framework/NativeVP9"]);
    envpath("DM_PSO_USER",[docs stringByAppendingPathComponent:@"pso_user.tsv"]);envpath("TMPDIR",NSTemporaryDirectory());chdir(docs.fileSystemRepresentation);
    setenv("DM_PROBE_INTERACTIVE","1",1);setenv("DM_NVN_SYNC","1",1);setenv("DM_PRESENT_HZ","60",1);
    NSError *audioError=nil;[[AVAudioSession sharedInstance]setCategory:AVAudioSessionCategoryPlayback error:&audioError];[[AVAudioSession sharedInstance]setActive:YES error:&audioError];
    if(audioError)fprintf(stderr,"audio session: %s\n",audioError.description.UTF8String);
    // 预留失败留在资源界面，不能让玩家每次点击都直接退出。
    if(!probe_memory_prepare()){self.notice.text=@"可用虚拟地址空间不足，尚未启动游戏。请关闭本应用后重试，并导出 boot.txt 排查。";return;}
    pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,64*1024*1024);pthread_t thread;
    int error=pthread_create(&thread,&attr,game_thread,NULL);pthread_attr_destroy(&attr);
    if(error){self.notice.text=[NSString stringWithFormat:@"游戏线程启动失败：%d",error];return;}
    pthread_detach(thread);self.started=YES;self.start.hidden=YES;self.notice.hidden=YES;pad.hidden=NO;UIApplication.sharedApplication.idleTimerDisabled=YES;
    __atomic_store_n(&probe_keyboard_ready,1,__ATOMIC_RELEASE);
}
@end
// 新版 iOS 在创建场景时要求 Scene 生命周期；窗口必须绑定真实 UIWindowScene。
@interface DQScene : UIResponder <UIWindowSceneDelegate>
@property UIWindow *window;
@end
@implementation DQScene
-(void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    if(![scene isKindOfClass:UIWindowScene.class])return;
    self.window=[[UIWindow alloc]initWithWindowScene:(UIWindowScene *)scene];
    root=[DQController new];self.window.rootViewController=root;[self.window makeKeyAndVisible];
}
-(void)sceneWillResignActive:(UIScene *)scene{[pad clear];__atomic_store_n(&probe_keyboard_ready,0,__ATOMIC_RELEASE);}
-(void)sceneDidBecomeActive:(UIScene *)scene{[pad clear];__atomic_store_n(&probe_keyboard_ready,1,__ATOMIC_RELEASE);}
@end
@interface DQApp : UIResponder <UIApplicationDelegate>
@end
@implementation DQApp
-(BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)options{return YES;}
@end
int main(int argc,char **argv){@autoreleasepool{return UIApplicationMain(argc,argv,nil,NSStringFromClass(DQApp.class));}}
