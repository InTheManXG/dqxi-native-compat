// 键盘边界测试不弹 UI：覆盖编码、终止符、容量、长度和禁用字符。
#define PROBE_KEYBOARD_TEST
#include "probe_keyboard.m"
#include <assert.h>
int main(void) { @autoreleasepool {
    NSData *data=keyboard_encode(@"勇者",0,6); assert(data.length==6); const uint16_t *p=data.bytes; assert(p[0]==0x52c7 && p[1]==0x8005 && !p[2]);
    assert(!keyboard_encode(@"勇者",0,5)); assert(keyboard_encode(@"勇者",1,7).length==7); assert(!keyboard_encode(@"勇者",1,6));
    assert(keyboard_allowed(@"勇者",0,0,1,6)); assert(!keyboard_allowed(@"",0,0,1,6)); assert(!keyboard_allowed(@"1234567",0,0,1,6));
    assert(!keyboard_allowed(@"a@b",0,256,1,8)); assert(!keyboard_allowed(@"a b",0,2,1,8)); assert(!keyboard_allowed(@"1",0,64,1,8));
    assert(keyboard_allowed(@"123",1,0,1,8)); assert(!keyboard_allowed(@"abc",1,0,1,8)); assert(!keyboard_allowed(@"a\nb",0,0,1,8));
    puts("PROBE keyboard encoding/limits/restrictions OK");
} }
