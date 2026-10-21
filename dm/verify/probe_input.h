// AppKit 键盘作为本地虚拟 Pro 手柄；键码按物理键位，不受输入法影响。
// AppKit keyboard as a local virtual Pro controller, independent of text input methods.
#pragma once
#include <stdint.h>
// 下标直接对应 Horizon 按钮位，保存配置时仍使用物理键码。
static const int probe_default_keys[24] = {36,53,8,9,3,5,12,14,15,17,35,48,123,126,124,125,0,13,2,1,38,34,37,40};
static uint64_t probe_key_button(unsigned key) {
    for (unsigned bit = 0; bit < 24; bit++) if (probe_default_keys[bit] == (int)key) return 1ull << bit;
    return 0;
}
static int probe_keys_valid(const int keys[24]) {
    for (int i = 0; i < 24; i++) {
        if (keys[i] < 0 || keys[i] >= 128) return 0;
        for (int j = 0; j < i; j++) if (keys[i] == keys[j]) return 0;
    }
    return 1;
}
static uint64_t probe_mapped_buttons(const int keys[24], const unsigned char pressed[128]) {
    uint64_t buttons = 0;
    for (unsigned bit = 0; bit < 24; bit++) if (pressed[keys[bit]]) buttons |= 1ull << bit;
    return buttons;
}
static uint64_t probe_stick(uint64_t buttons, unsigned shift) {
    unsigned b = (unsigned)(buttons >> shift) & 15;
    int x = (int)((b >> 2) & 1) - (int)(b & 1), y = (int)((b >> 1) & 1) - (int)((b >> 3) & 1);
    // 相反方向抵消；斜向保持圆形最大幅度，避免比单轴移动更快。
    int scale = x && y ? 23170 : 32767;
    return (uint64_t)(uint32_t)(x * scale) | ((uint64_t)(uint32_t)(y * scale) << 32);
}
extern uint64_t probe_keyboard_buttons;
extern int probe_keyboard_ready;

extern int probe_keyboard_modal;

// 同一快照包含按钮与双摇杆，避免采样线程读到跨帧组合。
void probe_controller_read(uint64_t state[3]);

static void probe_combined_input(uint64_t keyboard, const uint64_t controller[3], uint64_t state[3]) {
    state[0] = keyboard | controller[0];
    for (unsigned i = 1; i <= 2; i++) {
        unsigned shift = 16 + (i - 1) * 4;
        state[i] = probe_stick(keyboard, shift);
        // 键盘接管一个摇杆时同时替换方向位，避免模拟轴向左、按钮位却向右。
        if (state[i]) state[0] = (state[0] & ~(15ull << shift)) | (keyboard & (15ull << shift));
        else state[i] = controller[i];
    }
}
