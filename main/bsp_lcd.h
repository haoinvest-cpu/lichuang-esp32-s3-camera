#pragma once

#include <stdint.h>
#include "esp_err.h"

// 初始化 SPI3 + ST7789 面板 + 背光。必须在 bsp_i2c_init()/pca9557_init() 之后调用
// （片选走 PCA9557 bit0，背光和 CS 都依赖扩展芯片已就绪）。
esp_err_t bsp_lcd_init(void);

// 画一块 RGB565 位图。数据按面板字节序（大端）给出——摄像头帧可直接传。
esp_err_t bsp_lcd_draw_bitmap(int x_start, int y_start, int x_end, int y_end, const void *data);

// 整屏填充。rgb565 用逻辑值（0xF800 = 红），函数内部转换字节序。
esp_err_t bsp_lcd_fill(uint16_t rgb565);

// 整屏黑底 + 居中两行提示（line2 可为 NULL）。
esp_err_t bsp_lcd_message(const char *line1, const char *line2, uint16_t rgb565);

// 逻辑 RGB565 -> 面板字节序
static inline uint16_t bsp_lcd_color(uint16_t c)
{
    return (uint16_t)((c >> 8) | (c << 8));
}

esp_err_t bsp_lcd_backlight_set(int percent);
