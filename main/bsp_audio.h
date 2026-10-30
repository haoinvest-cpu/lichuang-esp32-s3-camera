// 最小音频输出：I2S TX + ES8311 DAC，只为拍照快门声。
// 引脚/参数出处见 bsp_audio.c 顶部注释（全部查自 Muse 定版固件树）。
#pragma once

#include <stdbool.h>

#include "esp_err.h"

// 初始化 I2S TX + ES8311 播放通道，并打开 PCA9557 的功放使能。
// 必须在 bsp_i2c_init() 与 pca9557_init() 之后调用。
// 任一步失败：打警告、模块置不可用、返回错误；相机照常工作，绝不崩溃。
esp_err_t bsp_audio_init(void);

// 初始化是否成功且播放通道可用。
bool bsp_audio_available(void);

// 播放代码合成的两段式快门声（“咔” + 70ms 静音 + “嚓”，总长约 113ms）。
// 不可用时立即返回；正常阻塞约 120~250ms。
void bsp_audio_play_shutter(void);
