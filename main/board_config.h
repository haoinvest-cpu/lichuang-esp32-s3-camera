// 板级引脚与常量。
// 每一个数字都能在 ~/camera-app/FACTS.md 里找到出处（本文件注释里给的是缩写 + 行号）。
//   SZP_H  = 官方例程 07-lcd_camera/main/esp32_s3_szp.h
//   SZP_C  = 官方例程 07-lcd_camera/main/esp32_s3_szp.c
//   SD_C   = 官方例程 03-micro_sd/main/main.c
//   MUSE_* = ~/muse-esp32-s3-lichuang（只读参考）
#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"

// ---------------- I2C0：摄像头 SCCB / PCA9557 / 触摸共用 ----------------
// SZP_H:21-25；MUSE_MD:8
#define BSP_I2C_PORT        0
#define BSP_I2C_SDA_PIN     GPIO_NUM_1
#define BSP_I2C_SCL_PIN     GPIO_NUM_2
#define BSP_I2C_FREQ_HZ     100000

// ---------------- IO 扩展芯片 PCA9557（板上丝印 TCA9557） ----------------
// SZP_H:146-155、SZP_C:213-219。寄存器与 PCA9557 兼容，按 PCA9557 写。
#define PCA9557_ADDR            0x19
#define PCA9557_REG_INPUT       0x00
#define PCA9557_REG_OUTPUT      0x01
#define PCA9557_REG_POLARITY    0x02
#define PCA9557_REG_CONFIG      0x03

#define PCA9557_BIT_LCD_CS      0   // 0 = 选中 LCD（低有效）
#define PCA9557_BIT_PA_EN       1   // 音频功放使能
#define PCA9557_BIT_DVP_PWDN    2   // 0 = 摄像头上电

// 上电默认：LCD_CS=1、PA_EN=0、PWDN=1（摄像头先断电），SZP_C:216/218
#define PCA9557_OUTPUT_DEFAULT  0x05
#define PCA9557_CONFIG_DEFAULT  0xF8   // bit0..2 输出，其余输入

// ---------------- LCD：ST7789 2.0"，面板 240x320，旋转后 320x240 ----------------
// SZP_H:169-184、SZP_C:339-363；MUSE_MD:9；MUSE_C:53-61
#define BSP_LCD_SPI_HOST        SPI3_HOST
#define BSP_LCD_PIN_MOSI        GPIO_NUM_40
#define BSP_LCD_PIN_CLK         GPIO_NUM_41
#define BSP_LCD_PIN_DC          GPIO_NUM_39
#define BSP_LCD_PIN_RST         GPIO_NUM_NC   // 无硬复位脚，走 SWRESET
#define BSP_LCD_PIN_CS          GPIO_NUM_NC   // 片选走 PCA9557 bit0
#define BSP_LCD_PIN_BACKLIGHT   GPIO_NUM_42   // 低电平点亮：LEDC output_invert
#define BSP_LCD_H_RES           320
#define BSP_LCD_V_RES           240
// 官方例程 80MHz（SZP_H:169）；Muse 有实机验证的是 40MHz（MUSE_C:220）。取 40MHz，见 FACTS §2。
#define BSP_LCD_PIXEL_CLOCK_HZ  (40 * 1000 * 1000)
#define BSP_LCD_SPI_MODE        2

// ---------------- 摄像头 GC0308（DVP 8 位） ----------------
// SZP_H:200-219
#define CAM_PIN_PWDN    -1          // 由 PCA9557 bit2 控制
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    GPIO_NUM_5
#define CAM_PIN_SIOD    GPIO_NUM_1  // 复用 I2C0
#define CAM_PIN_SIOC    GPIO_NUM_2
#define CAM_PIN_D7      GPIO_NUM_9
#define CAM_PIN_D6      GPIO_NUM_4
#define CAM_PIN_D5      GPIO_NUM_6
#define CAM_PIN_D4      GPIO_NUM_15
#define CAM_PIN_D3      GPIO_NUM_17
#define CAM_PIN_D2      GPIO_NUM_8
#define CAM_PIN_D1      GPIO_NUM_18
#define CAM_PIN_D0      GPIO_NUM_16
#define CAM_PIN_VSYNC   GPIO_NUM_3
#define CAM_PIN_HREF    GPIO_NUM_46
#define CAM_PIN_PCLK    GPIO_NUM_7

// XCLK：官方例程 24MHz（SZP_H:219），乐鑫 FAQ 说 GC0308 最大稳定 20MHz。本期 20MHz，见 FACTS §4.3。
#define CAM_XCLK_HZ     20000000

// 官方例程对 GC0308 打开水平镜像（SZP_C:485-487），目的是让取景画面在这块已经
// 硬件镜像过的屏上看起来正。照片沿用同一设置，保证"所见即所得"。
// 实拍发现照片左右反 → 改成 0（v3 实拍证据：文字/条码数字全是反的）。
#define CAM_HMIRROR     0

// ---------------- TF 卡：SDMMC 1 位 ----------------
// SD_C:7-9、SD_C:72-76
#define BSP_SD_PIN_CLK  GPIO_NUM_47
#define BSP_SD_PIN_CMD  GPIO_NUM_48
#define BSP_SD_PIN_D0   GPIO_NUM_21
#define BSP_SD_MOUNT_POINT "/sdcard"

// ---------------- 快门键 ----------------
// 板上没有独立快门键，只有 BOOT：GPIO0，低有效（MUSE_MD:15、MUSE_C:83）。
// 上电时按住会进下载模式；运行期正常可用。
#define BSP_BUTTON_SHUTTER GPIO_NUM_0

// ---------------- UI 颜色（RGB565） ----------------
#define UI_COLOR_BLACK  0x0000
#define UI_COLOR_WHITE  0xFFFF
#define UI_COLOR_RED    0xF800
