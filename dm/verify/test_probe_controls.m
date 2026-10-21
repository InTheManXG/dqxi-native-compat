// 覆盖键位配置与松键边界，菜单交互另外实机验收。
#define main probe_window_main
#include "probe_window.m"
#undef main
#include <assert.h>
uint64_t now_ns(void) { return 1000000000; }
uint64_t mtl_present_count(void) { return 0; }
int module_probe_main(int argc, char **argv) { (void)argc; (void)argv; return 0; }
int probe_can_close(void) { return 1; }
int main(void) {
    @autoreleasepool {
        memcpy(keys,probe_default_keys,sizeof keys); assert(probe_keys_valid(keys));
        assert([key_name(48) isEqualToString:@"Tab"] && [key_name(45) isEqualToString:@"N"]);
        assert([key_name(36) isEqualToString:@"Enter"] && [key_name(37) isEqualToString:@"L"]);
        keys[0] = 49; pressed[36] = 1; assert(!probe_mapped_buttons(keys,pressed));
        pressed[49] = 1; assert(probe_mapped_buttons(keys,pressed) == 1);
        pressed[49] = 0; assert(!probe_mapped_buttons(keys,pressed));
        keys[1] = 49; assert(!probe_keys_valid(keys));
        keys[1] = -1; assert(!probe_keys_valid(keys)); keys[1] = 128; assert(!probe_keys_valid(keys));
        probe_keyboard_buttons = 123; clear_input(); assert(!probe_keyboard_buttons);
        for (int i=0;i<128;i++) assert(!pressed[i]);
        key_menu = [NSMenu new];
        for (int i=0;i<24;i++) [key_menu addItemWithTitle:@"" action:nil keyEquivalent:@""];
        memcpy(keys,probe_default_keys,sizeof keys); refresh_keys();
        assert([[key_menu itemAtIndex:0].title isEqualToString:@"A 确认：Enter"]);
        assert([[key_menu itemAtIndex:11].title isEqualToString:@"−：Tab"]);
        // 用系统快照手柄走真实采样路径，覆盖热插拔/失焦门控及按键、模拟摇杆映射。
        GCController *controller = [GCController controllerWithExtendedGamepad];
        GCExtendedGamepad *pad = controller.extendedGamepad;
        uint64_t state[3];
        sample_controller(pad, YES); assert(controller_armed);
        [pad.buttonB setValue:1]; sample_controller(pad, YES); probe_controller_read(state); assert(state[0] == 1);
        [pad.buttonB setValue:0]; [pad.buttonA setValue:1];
        sample_controller(pad, YES); probe_controller_read(state); assert(state[0] == 2);
        sample_controller(pad, NO); probe_controller_read(state); assert(!(state[0] | state[1] | state[2]));
        sample_controller(pad, YES); probe_controller_read(state); assert(!state[0]);
        [pad.buttonA setValue:0]; sample_controller(pad, YES); assert(controller_armed);
        [pad.leftThumbstick setValueForXAxis:0.5 yAxis:0];
        [pad.rightThumbstick setValueForXAxis:0 yAxis:-1];
        sample_controller(pad, YES); probe_controller_read(state);
        assert((int32_t)state[1] > 13000 && (int32_t)state[1] < 14000);
        assert((int32_t)(state[2] >> 32) == -32767 && (state[0] & (1ull << 23)));
        sample_controller(nil, YES); probe_controller_read(state); assert(!(state[0] | state[1] | state[2]));
        sample_controller(pad, YES); probe_controller_read(state); assert(!(state[0] | state[1] | state[2]));
        [pad.leftThumbstick setValueForXAxis:0 yAxis:0]; [pad.rightThumbstick setValueForXAxis:0 yAxis:0];
        sample_controller(pad, YES); assert(controller_armed);
        GCControllerButtonInput *inputs[16] = {pad.buttonB,pad.buttonA,pad.buttonY,pad.buttonX,
            pad.leftThumbstickButton,pad.rightThumbstickButton,pad.leftShoulder,pad.rightShoulder,
            pad.leftTrigger,pad.rightTrigger,pad.buttonMenu,pad.buttonOptions,
            pad.dpad.left,pad.dpad.up,pad.dpad.right,pad.dpad.down};
        for (int i = 0; i < 12; i++) {
            assert(inputs[i]); [inputs[i] setValue:1]; sample_controller(pad, YES); probe_controller_read(state);
            assert(state[0] == (1ull << i)); [inputs[i] setValue:0];
        }
        const float directions[4][2] = {{-1,0},{0,1},{1,0},{0,-1}};
        for (int i = 0; i < 4; i++) {
            [pad.dpad setValueForXAxis:directions[i][0] yAxis:directions[i][1]];
            sample_controller(pad, YES); probe_controller_read(state); assert(state[0] == (1ull << (12+i)));
        }
        assert(!controller_stick(0.1f,0.1f) && !controller_stick(NAN,0));
        uint64_t diagonal = controller_stick(1,1);
        assert(hypot((int32_t)diagonal,(int32_t)(diagonal >> 32)) <= 32768);
        uint64_t physical[3] = {1ull << 18, controller_stick(1,0), controller_stick(0,1)};
        probe_combined_input(1ull << 16, physical, state);
        assert((int32_t)state[1] == -32767 && state[0] == (1ull << 16) && state[2] == physical[2]);
        probe_combined_input(1, physical, state);
        assert(state[0] == ((1ull << 18) | 1) && state[1] == physical[1]);
        clear_input(); probe_controller_read(state); assert(!(state[0] | state[1] | state[2]));
        puts("PROBE controls mapping/reset OK");
    }
}
