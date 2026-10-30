#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// 画面要施加的旋转角（顺时针）。取景/回看按该角度把相机帧转正后显示。
typedef enum {
    IMU_ROT_0   = 0,
    IMU_ROT_90  = 90,
    IMU_ROT_180 = 180,
    IMU_ROT_270 = 270,
} imu_rot_t;

// 探 QMI8658A（0x6A/0x6B）并写配置。必须在 bsp_i2c_init() 之后调用。
// 探不到/任一步失败：打一行警告、置不可用、返回错误；相机照常工作（等同 v3.2）。
esp_err_t imu_orient_init(void);

// 初始化是否成功（探到芯片且配置全部写成功）。
bool imu_orient_available(void);

// 读一帧加速度原始 LSB（ax/ay/az，小端 int16，±8g 时 4096 LSB/g）。
// 不可用或读失败返回 false，三个出参不动。
bool imu_orient_read(int16_t *ax, int16_t *ay, int16_t *az);

// 方向判定：输入原始 LSB，输出要对帧施加的旋转。映射表集中在这里。
imu_rot_t imu_orient_rotation(int16_t ax, int16_t ay, int16_t az);

// 取景方向跟踪（带迟滞）：读一次加速度，按 imu_orient_rotation() 得到候选方向；
// 只有候选方向持续 ORIENT_SWITCH_MS（300ms）、且新的主导板面内轴 >= 1.3 倍
// 另一板面内轴时才切换，避免 45° 附近来回跳。
// IMU 不可用 / 读失败：固定返回 IMU_ROT_0（横屏，等同 v3.2）。
// 只由取景任务调用即可（内部保存上次结果与候选计时，无锁）。
imu_rot_t imu_orient_poll_rotation(void);
