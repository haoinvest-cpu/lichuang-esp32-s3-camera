#pragma once

#include <stdbool.h>

#include "esp_camera.h"
#include "esp_err.h"

// 前提：bsp_i2c_init() + pca9557_init() + camera_power_on() 已经做完。
esp_err_t bsp_camera_init(void);

sensor_t *bsp_camera_sensor(void);
bool bsp_camera_ready(void);

// 预览 QVGA / 拍照 VGA 之间切换，内部就是 sensor_t::set_framesize。
esp_err_t bsp_camera_set_framesize(framesize_t fs);

// 拍照前改传感器参数的唯一入口（第 3 步 CCD 色调预设接这里）。本期是空实现。
void camera_apply_style_preset(void);
