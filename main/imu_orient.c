// QMI8658A 六轴传感器：只取加速度，用来判断取景时板子的握持方向。
// 总线是 bsp_i2c_init() 建好的 I2C0（与摄像头 SCCB / PCA9557 共用），这里只用
// i2c_master_get_bus_handle(0) 取句柄，绝不重初始化总线。

#include "imu_orient.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "imu";

#define IMU_I2C_PORT      0
#define IMU_ADDR_A        0x6A      // SA0 接地
#define IMU_ADDR_B        0x6B      // SA0 接高
#define IMU_WHO_AM_I_VAL  0x05

#define IMU_REG_WHO_AM_I  0x00
#define IMU_REG_CTRL1     0x02
#define IMU_REG_CTRL2     0x03
#define IMU_REG_CTRL7     0x08
#define IMU_REG_AX_L      0x35      // AX_L,AX_H,AY_L,AY_H,AZ_L,AZ_H

#define IMU_CTRL1_VAL     0x02      // 参考配置（见规格）
#define IMU_CTRL2_VAL     0x25      // ±8g，约 119Hz
#define IMU_CTRL7_VAL     0x01      // 只开加速度计

static i2c_master_dev_handle_t s_dev = NULL;
static uint8_t s_addr = 0;
static bool s_ok = false;

static esp_err_t imu_read_reg_to(uint8_t reg, uint8_t *val, int timeout_ms)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, timeout_ms);
}

static esp_err_t imu_write_reg(uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 1000);
}

// 探不到/写失败后的统一收尾：放掉设备句柄，标记不可用。
static void imu_disable(void)
{
    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    s_ok = false;
}

bool imu_orient_available(void)
{
    return s_ok;
}

esp_err_t imu_orient_init(void)
{
    imu_disable();
    s_addr = 0;

    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_master_get_bus_handle(IMU_I2C_PORT, &bus);
    if (err != ESP_OK || !bus) {
        ESP_LOGW(TAG, "IMU disabled: no I2C%d bus (%s)", IMU_I2C_PORT, esp_err_to_name(err));
        return ESP_ERR_NOT_FOUND;
    }

    // 两个可能的地址都探：WHO_AM_I(0x00) == 0x05 才算命中。
    const uint8_t addrs[2] = {IMU_ADDR_A, IMU_ADDR_B};
    uint8_t who = 0;
    bool found = false;

    for (int i = 0; i < 2 && !found; i++) {
        const i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addrs[i],
            .scl_speed_hz = 100000,
        };
        i2c_master_dev_handle_t dev = NULL;
        err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "probe 0x%02X: add_device failed (%s)", addrs[i], esp_err_to_name(err));
            continue;
        }
        s_dev = dev;

        // 探测用短超时：两个地址都没有芯片时最多拖 2x100ms，不拖慢开机
        err = imu_read_reg_to(IMU_REG_WHO_AM_I, &who, 100);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "probe 0x%02X: WHO_AM_I read failed (%s)", addrs[i], esp_err_to_name(err));
            imu_disable();
            continue;
        }
        ESP_LOGI(TAG, "probe 0x%02X: WHO_AM_I=0x%02X", addrs[i], who);
        if (who != IMU_WHO_AM_I_VAL) {
            ESP_LOGW(TAG, "probe 0x%02X: not QMI8658A (want 0x%02X)", addrs[i], IMU_WHO_AM_I_VAL);
            imu_disable();
            continue;
        }
        s_addr = addrs[i];
        found = true;
    }

    if (!found) {
        ESP_LOGW(TAG, "IMU not found on I2C%d (0x%02X/0x%02X), orientation disabled",
                 IMU_I2C_PORT, IMU_ADDR_A, IMU_ADDR_B);
        return ESP_ERR_NOT_FOUND;
    }

    // 逐项配置并打结果，哪一步失败就整体放弃（相机照常工作）。
    const struct {
        uint8_t reg;
        uint8_t val;
        const char *name;
    } steps[] = {
        {IMU_REG_CTRL1, IMU_CTRL1_VAL, "CTRL1"},
        {IMU_REG_CTRL2, IMU_CTRL2_VAL, "CTRL2"},
        {IMU_REG_CTRL7, IMU_CTRL7_VAL, "CTRL7"},
    };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        err = imu_write_reg(steps[i].reg, steps[i].val);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "IMU @0x%02X %s=0x%02X failed (%s), orientation disabled",
                     s_addr, steps[i].name, steps[i].val, esp_err_to_name(err));
            imu_disable();
            return err;
        }
        ESP_LOGI(TAG, "IMU @0x%02X %s(0x%02X)=0x%02X ok",
                 s_addr, steps[i].name, steps[i].reg, steps[i].val);
    }

    vTaskDelay(pdMS_TO_TICKS(50));      // 等第一次采样有效
    s_ok = true;
    ESP_LOGI(TAG, "IMU QMI8658A ready @0x%02X (accel only, ±8g)", s_addr);
    return ESP_OK;
}

bool imu_orient_read(int16_t *ax, int16_t *ay, int16_t *az)
{
    if (!s_ok || !s_dev || !ax || !ay || !az) {
        return false;
    }
    uint8_t raw[6];
    esp_err_t err = ESP_OK;
    for (int i = 0; i < 6; i++) {
        const uint8_t reg = (uint8_t)(IMU_REG_AX_L + i);
        err = i2c_master_transmit_receive(s_dev, &reg, 1, &raw[i], 1, 1000);
        if (err != ESP_OK) {
            break;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "accel read failed (%s)", esp_err_to_name(err));
        return false;
    }
    *ax = (int16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
    *ay = (int16_t)((uint16_t)raw[2] | ((uint16_t)raw[3] << 8));
    *az = (int16_t)((uint16_t)raw[4] | ((uint16_t)raw[5] << 8));
    return true;
}

// --------------------------------------------------------------------------
// 方向判定表（唯一一处）：2026-10-30 diag2 串口实测标定，数据见函数内注释。
// --------------------------------------------------------------------------
imu_rot_t imu_orient_rotation(int16_t ax, int16_t ay, int16_t az)
{
    // 2026-10-30 diag2 实测标定（四向各一组稳定读数，±8g 即 4096/g）：
    //   横（正常握持） ax≈+3550 ay≈+500  -> 不转
    //   竖             ax≈+400  ay≈+4150 -> 90CW
    //   反横           ax≈-3750 ay≈+470  -> 180
    //   反竖           ax≈-620  ay≈-3290 -> 270CW
    // az 是板面法向分量（手持倾斜时可达 +2000），只在面内分量太弱时
    // 判"近乎平放"，不参与方向选择（旧版让 az 抢方向是坑之一）。
    (void)az;
    const int iax = (ax < 0) ? -(int)ax : (int)ax;
    const int iay = (ay < 0) ? -(int)ay : (int)ay;

    if (iax < 1200 && iay < 1200) {
        return IMU_ROT_0;       // 面内重力太弱（近乎平放）：不转
    }
    if (iax >= iay) {           // ax 主导 = 横向两种握持
        return (ax > 0) ? IMU_ROT_0 : IMU_ROT_180;
    }
    return (ay > 0) ? IMU_ROT_90 : IMU_ROT_270;   // ay 主导 = 竖向两种握持
}

// --------------------------------------------------------------------------
// 取景方向跟踪：把上面那张表包一层迟滞，避免 45° 附近来回跳。
//   - 候选方向与当前不同才计时；
//   - 新主导轴必须 >= 1.3 倍另一条板面内轴（ratio = 13/10）；
//   - 候选持续 300ms 才真正切换。
// 平放（|az| 最大）时按表得到 0，不做比例判断（两个面内轴都接近 0）。
// --------------------------------------------------------------------------
#define ORIENT_SWITCH_MS   300
#define ORIENT_RATIO_NUM   13
#define ORIENT_RATIO_DEN   10

static imu_rot_t s_tracked_rot = IMU_ROT_0;     // 当前生效方向
static imu_rot_t s_pending_rot = IMU_ROT_0;     // 候选方向
static bool s_pending = false;
static int64_t s_pending_since_us = 0;

imu_rot_t imu_orient_poll_rotation(void)
{
    if (!imu_orient_available()) {
        s_tracked_rot = IMU_ROT_0;      // IMU 不可用：固定横屏，等同 v3.2
        s_pending = false;
        return s_tracked_rot;
    }

    int16_t ax = 0, ay = 0, az = 0;
    if (!imu_orient_read(&ax, &ay, &az)) {
        // 偶发读失败：保持当前方向，不闪一下
        s_pending = false;
        return s_tracked_rot;
    }

    const imu_rot_t cand = imu_orient_rotation(ax, ay, az);
    if (cand == s_tracked_rot) {
        s_pending = false;
        return s_tracked_rot;
    }

    const int iax = (ax < 0) ? -(int)ax : (int)ax;
    const int iay = (ay < 0) ? -(int)ay : (int)ay;
    const int iaz = (az < 0) ? -(int)az : (int)az;

    int dom = 0;        // 主导板面内轴分量
    int other = 0;      // 另一条板面内轴分量
    if (!(iaz > iax && iaz > iay)) {    // 不是平放
        if (iax >= iay) {
            dom = iax;
            other = iay;
        } else {
            dom = iay;
            other = iax;
        }
        // 45° 附近两条轴差不多：新方向不够明确，候选作废
        if (dom * ORIENT_RATIO_DEN < other * ORIENT_RATIO_NUM) {
            s_pending = false;
            return s_tracked_rot;
        }
    }

    const int64_t now = esp_timer_get_time();
    if (!s_pending || s_pending_rot != cand) {
        s_pending = true;
        s_pending_rot = cand;
        s_pending_since_us = now;
        return s_tracked_rot;
    }
    if (now - s_pending_since_us >= (int64_t)ORIENT_SWITCH_MS * 1000) {
        s_tracked_rot = cand;
        s_pending = false;
        ESP_LOGI(TAG, "preview rotation -> %d deg", (int)cand);
    }
    return s_tracked_rot;
}
