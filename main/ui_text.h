// 极简 5x7 点阵文字，只为了在屏上打几句提示（无卡、拍照失败）。
// 字形用 7 行 × 5 列的 '0'/'1' 字符串写，肉眼可核对，避免手算位图出错。
#pragma once

#include <stdint.h>

// fb 指向 RGB565 帧缓冲，px 是**已按面板字节序转换好**的像素值（见 bsp_lcd_color()）。
// scale 是整数放大倍数（2 或 3 比较好看）。
void ui_text_draw(uint16_t *fb, int fb_w, int fb_h, int x, int y,
                  const char *s, uint16_t px, int scale);

// 单行宽度（像素），用于居中
int ui_text_width(const char *s, int scale);

#define UI_TEXT_H(scale) (7 * (scale))
