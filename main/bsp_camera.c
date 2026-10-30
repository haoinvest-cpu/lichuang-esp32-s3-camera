#include "bsp_camera.h"

#include "board_config.h"

#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "camera";

static sensor_t *s_sensor = NULL;

esp_err_t bsp_camera_init(void)
{
    camera_config_t config = {0};

    // 官方例程 07-lcd_camera 的完整配置，SZP_C:447-473。
    // S3 的 XCLK 由 LCD_CAM 外设产生，ledc_* 这两项只是接口占位。
    config.ledc_channel = LEDC_CHANNEL_1;
    config.ledc_timer = LEDC_TIMER_1;

    config.pin_pwdn = CAM_PIN_PWDN;      // -1，实际由 PCA9557 bit2 供电
    config.pin_reset = CAM_PIN_RESET;    // -1，未接
    config.pin_xclk = CAM_PIN_XCLK;
    config.pin_d0 = CAM_PIN_D0;
    config.pin_d1 = CAM_PIN_D1;
    config.pin_d2 = CAM_PIN_D2;
    config.pin_d3 = CAM_PIN_D3;
    config.pin_d4 = CAM_PIN_D4;
    config.pin_d5 = CAM_PIN_D5;
    config.pin_d6 = CAM_PIN_D6;
    config.pin_d7 = CAM_PIN_D7;
    config.pin_vsync = CAM_PIN_VSYNC;
    config.pin_href = CAM_PIN_HREF;
    config.pin_pclk = CAM_PIN_PCLK;

    // SCCB 复用已经建好的 I2C0：sda=-1 时 esp32-camera 不再自己装总线，
    // 而是 i2c_master_get_bus_handle(0)（SZP_C:462-464、camera.c:191-196、sccb-ng.c:92）。
    config.pin_sccb_sda = -1;
    config.pin_sccb_scl = CAM_PIN_SIOC;  // 仅旧驱动分支会用到，这里只是把引脚写上
    config.sccb_i2c_port = BSP_I2C_PORT;

    config.xclk_freq_hz = CAM_XCLK_HZ;
    config.pixel_format = PIXFORMAT_RGB565;   // GC0308 无硬件 JPEG（FACTS §4.1）
    config.frame_size = FRAMESIZE_VGA;        // 帧缓冲在 init 时按此尺寸分配，必须一开始就给最大尺寸；
                                              // 运行中 set_framesize 到 VGA 会溢出小缓冲（FB-OVF 实测教训）。
                                              // 预览由软件把 VGA 帧缩小到 320x240 上屏。
    config.jpeg_quality = 12;                 // 对 RGB565 无效（GC0308 set_quality 是 dummy）
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s (0x%x)", esp_err_to_name(err), err);
        return err;
    }

    s_sensor = esp_camera_sensor_get();
    if (!s_sensor) {
        ESP_LOGE(TAG, "esp_camera_sensor_get returned NULL");
        return ESP_FAIL;
    }

    if (s_sensor->id.PID == GC0308_PID) {
        // 官方例程只对 GC0308 打开水平镜像（SZP_C:485-487）。
        s_sensor->set_hmirror(s_sensor, CAM_HMIRROR);
    } else {
        ESP_LOGW(TAG, "unexpected sensor PID 0x%04x (GC0308 is 0x%02x)",
                 s_sensor->id.PID, GC0308_PID);
    }

    ESP_LOGI(TAG, "sensor PID=0x%04x framesize=%d xclk=%dHz", s_sensor->id.PID,
             (int)s_sensor->status.framesize, s_sensor->xclk_freq_hz);
    return ESP_OK;
}

sensor_t *bsp_camera_sensor(void)
{
    return s_sensor;
}

bool bsp_camera_ready(void)
{
    return s_sensor != NULL;
}

esp_err_t bsp_camera_set_framesize(framesize_t fs)
{
    if (!s_sensor) {
        return ESP_ERR_INVALID_STATE;
    }
    int ret = s_sensor->set_framesize(s_sensor, fs);
    if (ret != 0) {
        ESP_LOGE(TAG, "set_framesize(%d) failed: %d", (int)fs, ret);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void camera_apply_style_preset(void)
{
    // ---- 第 3 步：CCD 色调预设从这里的空函数接进去 ----
    // 拍照前会被调用一次（bsp_camera 已初始化、帧率/分辨率已切到 VGA）。
    // GC0308 上真正生效的调节项（FACTS §4.2）：
    //   set_contrast / set_saturation / set_sharpness / set_wb_mode / set_ae_level /
    //   set_special_effect / set_hmirror / set_vflip
    // 注意 set_brightness 在 GC0308 上是 set_dummy 空实现，别用它。
    // 只调色调参数，不碰画面内容。
    //
    // sensor_t *s = bsp_camera_sensor();
    // if (s) { s->set_contrast(s, 0); s->set_saturation(s, 1); ... }
}
