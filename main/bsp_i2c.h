#pragma once

#include <stdint.h>
#include "esp_err.h"

// 建 I2C0 总线（摄像头 SCCB 会通过 i2c_master_get_bus_handle(0) 复用它）
esp_err_t bsp_i2c_init(void);

// PCA9557 侧：输出寄存器默认值 + 方向配置
esp_err_t pca9557_init(void);
esp_err_t pca9557_set_bit(uint8_t bit, int level);

// 摄像头电源：bit2 拉低 = 上电
esp_err_t camera_power_on(void);
