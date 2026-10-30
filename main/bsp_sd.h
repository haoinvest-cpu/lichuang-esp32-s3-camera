#pragma once

#include <stdbool.h>

#include "esp_err.h"

// 挂载板载 microSD（1 位 SD 模式，FAT32）。
// format_if_mount_failed 固定为 false：挂载失败只返回错误，绝不动用户的卡。
esp_err_t bsp_sd_mount(void);

bool bsp_sd_mounted(void);
