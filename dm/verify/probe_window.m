// 隔离探针的原生 Metal 窗口；只显示游戏真正提交的帧，不绘制替代启动画面。
// Native Metal window for the isolated probe; show only frames submitted by the game, never a substitute splash screen.
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <GameController/GameController.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include "probe_input.h"
uint64_t probe_keyboard_buttons;
int probe_keyboard_ready;
int probe_keyboard_modal;
static CAMetalLayer *layer;
static NSWindow *window;
static int probe_argc;
static char **probe_argv;
uint64_t g_t0_ns;
extern uint64_t now_ns(void);
extern int module_probe_main(int, char **);
extern int probe_can_close(void);
static NSUserDefaults *settings;
static NSMenu *key_menu;
static NSMenuItem *fps_item;
static int keys[24], recording = -1, suppress_key = -1;
static unsigned char pressed[128];
static BOOL show_fps;
static pthread_mutex_t controller_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t controller_state[3];
static GCController *active_controller;
static BOOL controller_armed, menu_tracking;
void probe_controller_read(uint64_t state[3]) {
    pthread_mutex_lock(&controller_mutex);
    memcpy(state, controller_state, sizeof controller_state);
    pthread_mutex_unlock(&controller_mutex);
}
static void publish_controller(const uint64_t state[3]) {
    pthread_mutex_lock(&controller_mutex);
    memcpy(controller_state, state, sizeof controller_state);
    pthread_mutex_unlock(&controller_mutex);
}
static uint64_t controller_stick(float x, float y) {
    if (!isfinite(x) || !isfinite(y)) return 0;
    // 圆形死区抑制漂移，剩余行程线性缩放，斜向不超过最大幅度。
    float length = hypotf(x, y);
    if (length <= 0.15f) return 0;
    float scale = (fminf(length, 1.f) - 0.15f) / (0.85f * length);
    int32_t sx = lroundf(x * scale * 32767), sy = lroundf(y * scale * 32767);
    return (uint64_t)(uint32_t)sx | ((uint64_t)(uint32_t)sy << 32);
}
static void sample_controller(GCExtendedGamepad *pad, BOOL allowed) {
    uint64_t state[3] = {0};
    if (pad && allowed) {
        // Apple 的 ABXY 按 Xbox 方位命名；按物理位置映射到 Switch，圆圈确认、叉返回。
        GCControllerButtonInput *buttons[16] = {pad.buttonB, pad.buttonA, pad.buttonY, pad.buttonX,
            pad.leftThumbstickButton, pad.rightThumbstickButton, pad.leftShoulder, pad.rightShoulder,
            pad.leftTrigger, pad.rightTrigger, pad.buttonMenu, pad.buttonOptions,
            pad.dpad.left, pad.dpad.up, pad.dpad.right, pad.dpad.down};
        for (unsigned i = 0; i < 16; i++) if (buttons[i].isPressed) state[0] |= 1ull << i;
        state[1] = controller_stick(pad.leftThumbstick.xAxis.value, pad.leftThumbstick.yAxis.value);
        state[2] = controller_stick(pad.rightThumbstick.xAxis.value, pad.rightThumbstick.yAxis.value);
        for (unsigned i = 1; i <= 2; i++) {
            int32_t x = (int32_t)state[i], y = (int32_t)(state[i] >> 32);
            unsigned shift = 16 + (i - 1) * 4;
            if (x < -16383) state[0] |= 1ull << shift;
            if (y > 16383) state[0] |= 1ull << (shift + 1);
            if (x > 16383) state[0] |= 1ull << (shift + 2);
            if (y < -16383) state[0] |= 1ull << (shift + 3);
        }
        // 重连、恢复焦点或关闭弹窗后先回中，防止仍按住的确认键误操作。
        if (!controller_armed) {
            controller_armed = !(state[0] || state[1] || state[2]);
            memset(state, 0, sizeof state);
        }
    } else controller_armed = NO;
    publish_controller(state);
}
static void poll_controller(void) {
    NSArray<GCController *> *controllers = GCController.controllers;
    if (!active_controller || ![controllers containsObject:active_controller]) {
        GCController *next = nil;
        for (GCController *candidate in controllers) if (candidate.extendedGamepad) { next = candidate; break; }
        if (next != active_controller) {
            active_controller = next; controller_armed = NO;
            fprintf(stderr, "[probe-input] controller=%s\n", next ? (next.vendorName ?: @"Unknown").UTF8String : "disconnected");
        }
    }
    sample_controller([active_controller.extendedGamepad capture],
        NSApp.isActive && window.isKeyWindow && !menu_tracking && recording < 0 &&
        !__atomic_load_n(&probe_keyboard_modal, __ATOMIC_ACQUIRE));
}
static const char *action_names[24] = {"A 确认","B 返回","X","Y","左摇杆按下","右摇杆按下","L","R","ZL","ZR","+ 菜单","−","十字键左","十字键上","十字键右","十字键下","左摇杆左","左摇杆上","左摇杆右","左摇杆下","右摇杆左","右摇杆上","右摇杆右","右摇杆下"};
static NSString *key_name(int key) {
    static const char *letters = "ASDFHGZXCV?BQWERYT123465=97-80]OU[IP?LJ'K;\\,/NM.";
    if (key >= 0 && key < (int)strlen(letters) && letters[key] != '?') return [NSString stringWithFormat:@"%c", letters[key]];
    switch (key) {
        case 36: return @"Enter"; case 48: return @"Tab"; case 49: return @"Space";
        case 51: return @"Delete"; case 53: return @"Esc";
        case 123: return @"←"; case 124: return @"→"; case 125: return @"↓"; case 126: return @"↑";
        default: return [NSString stringWithFormat:@"键码 %d", key];
    }
}
static void clear_input(void) {
    memset(pressed, 0, sizeof pressed);
    controller_armed = NO; publish_controller((uint64_t[3]){0});
    __atomic_store_n(&probe_keyboard_buttons, 0, __ATOMIC_RELAXED);
}
static void refresh_keys(void) {
    for (int bit = 0; bit < 24; bit++) [key_menu itemAtIndex:bit].title = [NSString stringWithFormat:@"%@：%@", @(action_names[bit]), key_name(keys[bit])];
}
static void save_keys(void) {
    NSMutableArray *values = [NSMutableArray new];
    for (int bit = 0; bit < 24; bit++) [values addObject:@(keys[bit])];
    [settings setObject:values forKey:@"keyboard"];
    refresh_keys();
}
extern uint64_t mtl_present_count(void);
@interface ProbeWindowDelegate : NSObject <NSWindowDelegate, NSApplicationDelegate>
@end
@implementation ProbeWindowDelegate
- (void)toggleFPS:(NSMenuItem *)sender {
    show_fps = !show_fps; sender.state = show_fps ? NSControlStateValueOn : NSControlStateValueOff;
    [settings setBool:show_fps forKey:@"showFPS"];
    if (!show_fps && recording < 0) window.title = @"DQXI S";
}
- (void)recordKey:(NSMenuItem *)sender {
    if (__atomic_load_n(&probe_keyboard_modal,__ATOMIC_ACQUIRE)) return;
    clear_input(); recording = (int)sender.tag;
    window.title = [NSString stringWithFormat:@"为 %@ 按新键（Esc 取消）", @(action_names[recording])];
}
- (void)resetKeys:(id)sender {
    (void)sender; clear_input(); recording = -1;
    memcpy(keys,probe_default_keys,sizeof keys); save_keys(); window.title = @"DQXI S";
}

- (BOOL)windowShouldClose:(NSWindow *)sender {
    (void)sender;
    // 保存期间拒绝关闭，完成后可重试；强制终止仍由存档事务恢复机制保护。
    if (!probe_can_close()) { NSBeep(); return NO; }
    return YES;
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
    (void)sender; return probe_can_close() ? NSTerminateNow : NSTerminateCancel;
}
@end
static ProbeWindowDelegate *window_delegate;
void *front_metal_layer(void) { return (__bridge void *)layer; }
static void *run_game(void *unused) {
    (void)unused;
    @autoreleasepool { exit(module_probe_main(probe_argc, probe_argv)); }
}
int main(int argc, char **argv) {
    @autoreleasepool {
        probe_argc = argc; probe_argv = argv; g_t0_ns = now_ns();
        [NSApplication sharedApplication];
        window_delegate = [ProbeWindowDelegate new];
        NSApp.delegate = window_delegate;
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1280, 720)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
            backing:NSBackingStoreBuffered defer:NO];
        window.title = @"DQXI S";
        window.delegate = window_delegate;
        layer = [CAMetalLayer layer]; layer.device = MTLCreateSystemDefaultDevice();
        if (!layer.device) return 2;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        window.contentView.wantsLayer = YES; window.contentView.layer = layer;
        // 窗口保持 16:9 逻辑尺寸，输出保留底座 UI 的 1080p，避免最后一步降采样到 720p。
        layer.drawableSize = CGSizeMake(1920, 1080);
        [window center]; [window makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
        settings = [[NSUserDefaults alloc] initWithSuiteName:@"cn.iamxg.dqxi-native"];
        memcpy(keys,probe_default_keys,sizeof keys);
        NSArray *saved = [settings arrayForKey:@"keyboard"];
        if (saved.count == 24) {
            int candidate[24]; BOOL valid = YES;
            for (int i = 0; i < 24; i++) {
                if (![saved[i] isKindOfClass:NSNumber.class] || [saved[i] doubleValue] != [saved[i] intValue]) { valid = NO; break; }
                candidate[i] = [saved[i] intValue];
            }
            if (valid && probe_keys_valid(candidate)) memcpy(keys,candidate,sizeof keys);
        }
        NSMenu *bar = [NSMenu new], *menu = [[NSMenu alloc] initWithTitle:@"DQXI S"];
        NSMenuItem *root = [[NSMenuItem alloc] initWithTitle:@"DQXI S" action:nil keyEquivalent:@""];
        root.submenu = menu; [bar addItem:root]; NSApp.mainMenu = bar;
        fps_item = [menu addItemWithTitle:@"显示帧率（窗口标题）" action:@selector(toggleFPS:) keyEquivalent:@""];
        fps_item.target = window_delegate; show_fps = [settings boolForKey:@"showFPS"];
        fps_item.state = show_fps ? NSControlStateValueOn : NSControlStateValueOff;
        NSMenuItem *key_item = [menu addItemWithTitle:@"键盘映射" action:nil keyEquivalent:@""];
        key_menu = [[NSMenu alloc] initWithTitle:@"键盘映射"]; key_item.submenu = key_menu;
        for (int bit = 0; bit < 24; bit++) {
            NSMenuItem *item = [key_menu addItemWithTitle:@"" action:@selector(recordKey:) keyEquivalent:@""];
            item.tag = bit; item.target = window_delegate;
        }
        [key_menu addItem:NSMenuItem.separatorItem];
        [key_menu addItemWithTitle:@"恢复默认键位" action:@selector(resetKeys:) keyEquivalent:@""].target = window_delegate;
        refresh_keys();
        [menu addItem:NSMenuItem.separatorItem];
        [menu addItemWithTitle:@"退出" action:@selector(terminate:) keyEquivalent:@"q"];
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp handler:^NSEvent *(NSEvent *event) {
            if (event.window != window || !window.isKeyWindow || __atomic_load_n(&probe_keyboard_modal, __ATOMIC_ACQUIRE)) return event;
            if (event.modifierFlags & NSEventModifierFlagCommand) { clear_input(); return event; }
            unsigned key = event.keyCode;
            // 录制用的按键必须完整松开才交给游戏，长按重复不能误触确认。
            if ((int)key == suppress_key) {
                if (event.type == NSEventTypeKeyUp) suppress_key = -1;
                return nil;
            }
            if (recording >= 0) {
                if (event.type == NSEventTypeKeyUp || event.isARepeat) return nil;
                if (key == 53) { suppress_key = key; recording = -1; window.title = @"DQXI S"; return nil; }
                if (key >= 128) { NSBeep(); return nil; }
                for (int bit = 0; bit < 24; bit++) if (bit != recording && keys[bit] == (int)key) {
                    window.title = [NSString stringWithFormat:@"%@ 已用于 %@，请换一个键（Esc 取消）", key_name(key), @(action_names[bit])];
                    NSBeep(); return nil;
                }
                keys[recording] = key; suppress_key = key; recording = -1; clear_input(); save_keys(); window.title = @"DQXI S";
                return nil;
            }
            BOOL mapped = NO;
            for (int bit = 0; bit < 24; bit++) mapped |= keys[bit] == (int)key;
            if (!mapped) return event;
            pressed[key] = event.type == NSEventTypeKeyDown;
            __atomic_store_n(&probe_keyboard_buttons,probe_mapped_buttons(keys,pressed),__ATOMIC_RELAXED);
            return nil;
        }];
        // 菜单接管键盘时可能吞掉松键事件，进入菜单就清空，防止角色持续移动。
        [[NSNotificationCenter defaultCenter] addObserverForName:NSMenuDidBeginTrackingNotification object:nil queue:nil usingBlock:^(NSNotification *note) {
            (void)note; menu_tracking = YES; clear_input();
        }];
        [[NSNotificationCenter defaultCenter] addObserverForName:NSMenuDidEndTrackingNotification object:nil queue:nil usingBlock:^(NSNotification *note) {
            (void)note; menu_tracking = NO;
        }];
        NSTimer *controller_timer = [NSTimer timerWithTimeInterval:0.008 repeats:YES block:^(NSTimer *timer) {
            (void)timer; poll_controller();
        }];
        [[NSRunLoop mainRunLoop] addTimer:controller_timer forMode:NSRunLoopCommonModes];
        __block uint64_t last_frames = mtl_present_count(), last_time = now_ns();
        [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer *timer) {
            (void)timer; uint64_t frames = mtl_present_count(), time = now_ns();
            if (show_fps && recording < 0 && time > last_time)
                window.title = [NSString stringWithFormat:@"DQXI S — %.1f FPS", (frames-last_frames)*1e9/(time-last_time)];
            last_frames = frames; last_time = time;
        }];
        [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidResignKeyNotification object:window queue:nil usingBlock:^(NSNotification *note) {
            (void)note; clear_input(); recording = -1; window.title = @"DQXI S";
        }];
        [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification object:window queue:nil usingBlock:^(NSNotification *note) {
            (void)note; exit(0);
        }];
        __atomic_store_n(&probe_keyboard_ready, 1, __ATOMIC_RELEASE);
        pthread_t thread;
        if (pthread_create(&thread, NULL, run_game, NULL)) return 2;
        pthread_detach(thread);
        [NSApp run];
    }
    return 0;
}
