// 同步 SDK 键盘桥：只替换系统输入界面，文本校验仍在原游戏线程执行。
// Synchronous SDK keyboard bridge; guest validation remains on the original game thread.
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
extern NSString *probe_ios_prompt(NSString *, NSString *, NSString *, BOOL, BOOL);
#else
#import <AppKit/AppKit.h>
#endif
#include <mach/mach.h>
#include "../host/hle.h"
#include "probe_input.h"
#include <stdio.h>
#include <string.h>

typedef struct { void *buffer; size_t bytes; } probe_keyboard_string;
static uint32_t keyboard_u32(const uint8_t *p) { uint32_t v; memcpy(&v,p,4); return v; }
static uint64_t keyboard_u64(const uint8_t *p) { uint64_t v; memcpy(&v,p,8); return v; }
static NSString *keyboard_text(const void *p, size_t units) {
    const uint16_t *s=p; size_t n=0; while(n<units && s[n])n++;
    return [[NSString alloc] initWithCharacters:s length:n] ?: @"";
}
static NSData *keyboard_encode(NSString *text, int utf8, size_t capacity) {
    NSData *bytes=[text dataUsingEncoding:utf8 ? NSUTF8StringEncoding : NSUTF16LittleEndianStringEncoding allowLossyConversion:NO];
    size_t zero=utf8 ? 1 : 2;
    if (!bytes || bytes.length+zero>capacity) return nil;
    NSMutableData *terminated=[bytes mutableCopy]; [terminated increaseLengthBy:zero]; return terminated;
}
static BOOL keyboard_allowed(NSString *s, uint32_t type, uint32_t mask, uint32_t min, uint32_t max) {
    if (s.length<min || s.length>max || [s rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location!=NSNotFound) return NO;
    NSString *forbidden=@"";
    if(mask&2)forbidden=[forbidden stringByAppendingString:@" "];
    if(mask&(4|256))forbidden=[forbidden stringByAppendingString:@"@"];
    if(mask&(8|256))forbidden=[forbidden stringByAppendingString:@"%"];
    if(mask&16)forbidden=[forbidden stringByAppendingString:@"/"];
    if(mask&(32|256))forbidden=[forbidden stringByAppendingString:@"\\"];
    if(mask&64)forbidden=[forbidden stringByAppendingString:@"0123456789"];
    if([s rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:forbidden]].location!=NSNotFound)return NO;
    if(type==1 && [s rangeOfCharacterFromSet:[[NSCharacterSet characterSetWithCharactersInString:@"0123456789"] invertedSet]].location!=NSNotFound)return NO;
    return YES;
}
#ifndef PROBE_KEYBOARD_TEST
HLE_FORCE("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", "native macOS synchronous software keyboard", uint32_t, probe_show_keyboard, probe_keyboard_string *output, const void *arguments) {
    // 布局来自当前 DQXI SDK 的 ShowKeyboard/ReadCloseResultAndString；其他 SDK 版本需重新核对。
    // Layout verified against this DQXI SDK, not a cross-version keyboard ABI.
    uint8_t arg[0x500]; probe_keyboard_string out; vm_size_t got=0;
    if(vm_read_overwrite(mach_task_self(),(vm_address_t)arguments,sizeof arg,(vm_address_t)arg,&got)!=KERN_SUCCESS || got!=sizeof arg ||
       vm_read_overwrite(mach_task_self(),(vm_address_t)output,sizeof out,(vm_address_t)&out,&got)!=KERN_SUCCESS || got!=sizeof out || !out.buffer || out.bytes<2 || out.bytes>0x10000)return 0x29f;
    uint32_t type=keyboard_u32(arg), mask=keyboard_u32(arg+0x1c), max=keyboard_u32(arg+0x3ac), min=keyboard_u32(arg+0x3b0);
    if(!max)max=500;
    int utf8=arg[0x3bd], password=keyboard_u32(arg+0x3b4)!=0;
    fprintf(stderr,"PROBE keyboard type=%u mask=%#x min=%u max=%u utf8=%d check=%u\n",type,mask,min,max,utf8,arg[0x3d0]);
    // shortcut: 下载码和多行键盘不在本次名字输入范围；遇到时取消，不能伪造有效输入。
    // Download-code and multiline keyboards need their own acceptance before enabling them.
    if(type>8 || mask&~0x17eu || max>500 || min>max || utf8>1 || arg[0x3bc])return 0x29f;
    uint16_t initial[501]={0}; uint32_t offset=keyboard_u32(arg+0x3c0), units=keyboard_u32(arg+0x3c4);
    uint64_t work=keyboard_u64(arg+0x4d0), work_size=keyboard_u64(arg+0x4d8);
    if(units>500 || offset>work_size || (uint64_t)units*2>work_size-offset)return 0x29f;
    if(units && (vm_read_overwrite(mach_task_self(),work+offset,units*2,(vm_address_t)initial,&got)!=KERN_SUCCESS || got!=units*2))return 0x29f;
    NSString *text=keyboard_text(initial,units), *message=keyboard_text(arg+0x1a8,257);
    NSString *title=keyboard_text(arg+0x24,65); if(!title.length)title=@"输入名字";
    uint32_t result=0x29f;
    // UI 只在主线程；游戏回调不能搬到主线程，因为它依赖客体 TLS。
    // Only the UI runs on the main thread; guest callbacks depend on the caller's TLS.
    __atomic_store_n(&probe_keyboard_modal,1,__ATOMIC_RELEASE);
    __atomic_store_n(&probe_keyboard_buttons,0,__ATOMIC_RELAXED);
    for(;;) {
        __block NSString *entered=nil;
#if TARGET_OS_IPHONE
        entered=probe_ios_prompt(title,message.length ? message : [NSString stringWithFormat:@"请输入 %u～%u 个字符",min,max],text,password,NO);
#else
        dispatch_sync(dispatch_get_main_queue(), ^{
            NSAlert *alert=[NSAlert new]; alert.messageText=title;
            alert.informativeText=message.length ? message : [NSString stringWithFormat:@"请输入 %u～%u 个字符",min,max];
            [alert addButtonWithTitle:@"确定"]; [alert addButtonWithTitle:@"取消"];
            NSTextField *field=password ? [[NSSecureTextField alloc] initWithFrame:NSMakeRect(0,0,320,28)] : [[NSTextField alloc] initWithFrame:NSMakeRect(0,0,320,28)];
            field.stringValue=text; alert.accessoryView=field; [alert.window setInitialFirstResponder:field];
            [NSApp activateIgnoringOtherApps:YES];
            if([alert runModal]==NSAlertFirstButtonReturn)entered=field.stringValue;
        });
#endif
        if(!entered)break;
        text=entered;
        NSData *encoded=keyboard_encode(text,utf8,out.bytes);
        if(!encoded || !keyboard_allowed(text,type,mask,min,max)) {message=@"长度或字符不符合游戏要求，请修改。";continue;}
        if(vm_write(mach_task_self(),(vm_address_t)out.buffer,(vm_offset_t)encoded.bytes,(mach_msg_type_number_t)encoded.length)!=KERN_SUCCESS)break;
        uint64_t callback=keyboard_u64(arg+0x3d8);
        if(arg[0x3d0] && callback) {
            size_t feedback_bytes=0; void *feedback=(void *)keyboard_u64(arg+0x4e0);
            probe_keyboard_string checked={out.buffer,encoded.length};
            uint32_t status=((uint32_t (*)(void *,size_t *,probe_keyboard_string *))callback)(feedback,&feedback_bytes,&checked);
            if(status) {
                message=@"游戏未接受这个名字，请修改。";
                if(status==2) {
                    __block BOOL accepted=NO;
#if TARGET_OS_IPHONE
                    accepted=probe_ios_prompt(@"游戏要求确认这个名字",@"",nil,NO,YES)!=nil;
#else
                    dispatch_sync(dispatch_get_main_queue(), ^{ NSAlert *alert=[NSAlert new];alert.messageText=@"游戏要求确认这个名字";[alert addButtonWithTitle:@"继续使用"];[alert addButtonWithTitle:@"重新输入"];accepted=[alert runModal]==NSAlertFirstButtonReturn; });
#endif
                    if(accepted){result=0;break;}
                }
                continue;
            }
        }
        result=0; break;
    }
    __atomic_store_n(&probe_keyboard_buttons,0,__ATOMIC_RELAXED);
    __atomic_store_n(&probe_keyboard_modal,0,__ATOMIC_RELEASE);
    fprintf(stderr,"PROBE keyboard completed result=%#x\n",result);
    return result;
}
#endif
