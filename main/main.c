// 立创实战派 ESP32-S3 相机应用（第 0/1/3 步）
// 取景（VGA 采集、按握持方向转正后缩小上屏）-> 按 BOOT 键拍一张 VGA 全尺寸照片 -> frame2jpg -> 存 TF 卡 -> 屏上回看 3 秒 -> 回取景。
// v3 追加：WiFi 网页相册（看图/下载/删除/遥控快门/实时取景）、屏上常驻 IP+张数状态行、BOOT 长按 5 秒清除配网。
// v4.1：成片不再旋转（照片就是传感器原图）；取景/回看按 IMU 方向自动转正；拍照瞬间播快门声。
// 板级事实见 ~/camera-app/FACTS.md；本文件里不出现任何往照片上叠字/时间日期的逻辑（状态行只画在 LCD 画布上）。

#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "bsp_audio.h"
#include "bsp_camera.h"
#include "bsp_i2c.h"
#include "bsp_lcd.h"
#include "bsp_sd.h"
#include "imu_orient.h"
#include "photo_store.h"
#include "ui_text.h"
#include "wifi_web.h"

#include "driver/gpio.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "img_converters.h"
#include "nvs_flash.h"

static const char *TAG = "app";

#define JPEG_QUALITY      88
#define REVIEW_HOLD_MS    3000
#define PREVIEW_WARN_MS   2500
#define PREVIEW_BUF_BYTES (192 * 1024)

static volatile bool s_shoot_request = false;
static bool s_lcd_ok = false;

// 送屏缓冲（320x240）与“用户视角画布”（横 320x240 / 竖 240x320，像素数相同）。
// 画布按当前握持方向把相机帧转正（含状态行文字），再整幅推给 LCD。
static uint16_t *s_lcd_buf = NULL;
static uint16_t *s_view_buf = NULL;

// 取景 JPEG 信箱：/preview.jpg 请求 -> camera_task 编码一帧放进 s_prev_buf
static uint8_t *s_prev_buf = NULL;
static size_t s_prev_len = 0;
static volatile bool s_prev_req = false;
static SemaphoreHandle_t s_prev_sem = NULL;     // 二值，初始为空
static SemaphoreHandle_t s_prev_mutex = NULL;


// --------------------------------------------------------------------------
// 按键：板上只有 BOOT（GPIO0，低有效）。见 FACTS §5。
// --------------------------------------------------------------------------
static void button_init(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << BSP_BUTTON_SHUTTER,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
}

// 网页「遥控快门」入口（wifi_web.c 调用）。
void camera_app_request_shoot(void)
{
    s_shoot_request = true;
}

static void button_task(void *arg)
{
    int held_ms = 0;
    bool long_done = false;     // 本次按压已触发长按清除 -> 松手不再拍照

    while (true) {
        bool down = (gpio_get_level(BSP_BUTTON_SHUTTER) == 0);

        if (down) {
            if (held_ms == 0) {
                vTaskDelay(pdMS_TO_TICKS(30));      // 消抖
                if (gpio_get_level(BSP_BUTTON_SHUTTER) != 0) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                    continue;
                }
                held_ms = 30;
            } else {
                held_ms += 20;
            }

            if (!long_done && held_ms >= 5000) {
                long_done = true;
                ESP_LOGI(TAG, "wifi credentials cleared by long press");
                wifi_web_forget();
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
        } else if (held_ms > 0) {
            // 松手：<1 秒算短按（拍照）；长按已经触发清除就不再拍
            if (!long_done && held_ms < 1000) {
                ESP_LOGI(TAG, "shutter pressed");
                s_shoot_request = true;
            }
            held_ms = 0;
            long_done = false;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// --------------------------------------------------------------------------
// 取景 JPEG 信箱（/preview.jpg 用）
// 协议：get 成功则持锁交给调用方，发送完必须调 camera_app_preview_release()。
// --------------------------------------------------------------------------
bool camera_app_get_preview_jpeg(const uint8_t **out, size_t *len, int timeout_ms)
{
    if (!out || !len || !s_prev_mutex || !s_prev_sem || !s_prev_buf) {
        return false;
    }
    if (xSemaphoreTake(s_prev_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }

    // 丢掉上一次超时后迟到的 give，避免本次误判为空帧
    xSemaphoreTake(s_prev_sem, 0);

    s_prev_len = 0;
    s_prev_req = true;

    bool ok = false;
    if (xSemaphoreTake(s_prev_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE && s_prev_len > 0) {
        *out = s_prev_buf;
        *len = s_prev_len;
        ok = true;
    }
    if (!ok) {
        s_prev_req = false;
        xSemaphoreGive(s_prev_mutex);
    }
    return ok;
}

void camera_app_preview_release(void)
{
    if (s_prev_mutex) {
        xSemaphoreGive(s_prev_mutex);
    }
}

// camera_task 每帧调用：有请求就编码一帧 VGA JPEG 放进信箱。
// 编码失败/超 192KB 时把长度清 0 并 give，让等待方按超时失败处理。
static void preview_mailbox_service(camera_fb_t *fb)
{
    if (!s_prev_req) {
        return;
    }
    uint8_t *jpg = NULL;
    size_t jpg_len = 0;
    s_prev_len = 0;
    if (fb && frame2jpg(fb, 75, &jpg, &jpg_len) && jpg) {
        if (jpg_len <= PREVIEW_BUF_BYTES) {
            memcpy(s_prev_buf, jpg, jpg_len);
            s_prev_len = jpg_len;
        }
    }
    if (jpg) {
        free(jpg);
    }
    s_prev_req = false;
    xSemaphoreGive(s_prev_sem);
}


// --------------------------------------------------------------------------
// 屏上提示
// --------------------------------------------------------------------------
static void show_message(const char *line1, const char *line2, uint16_t color, int hold_ms)
{
    if (!s_lcd_ok) {
        return;
    }
    bsp_lcd_message(line1, line2, color);
    if (hold_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(hold_ms));
    }
}

// --------------------------------------------------------------------------
// 拍照
// --------------------------------------------------------------------------
static void discard_frames(int count)
{
    for (int i = 0; i < count; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            esp_camera_fb_return(fb);
        }
    }
}

// --------------------------------------------------------------------------
// 取景/回看：把相机帧连同状态行合成到“用户视角画布”，再整幅送 LCD。
// 画布横握 320x240、竖握 240x320（像素数相同）。相机帧按当前 rot 顺时针
// 转正后最近邻缩进画布；送屏时再把画布按 rot 转回板子的逻辑帧（见下）。
// --------------------------------------------------------------------------
#define VIEW_MAX_SIDE BSP_LCD_H_RES     // 320：画布任一边的最大值

// 相机帧 -> 画布。逐像素反算源坐标（最近邻），每帧先建一次行/列查找表。
static void compose_view(const camera_fb_t *fb, imu_rot_t rot, uint16_t *dst, int cw, int ch)
{
    static int map_x[VIEW_MAX_SIDE];    // 横握：画布 x -> 源 x；竖握：画布 y -> 源 x
    static int map_y[VIEW_MAX_SIDE];    // 横握：画布 y -> 源行偏移；竖握：画布 x -> 源行偏移
    const uint16_t *src = (const uint16_t *)fb->buf;
    const int W = fb->width;
    const int H = fb->height;

    if (rot == IMU_ROT_90 || rot == IMU_ROT_270) {
        // 顺时针转 90/270 后长宽互换（480x640），再按最近邻铺进 240x320 画布。
        // 90CW  : dest(x',y') = src(x=y', y=H-1-x')
        // 270CW : dest(x',y') = src(x=W-1-y', y=x')
        for (int y = 0; y < ch; y++) {
            const int ry = y * W / ch;
            map_x[y] = (rot == IMU_ROT_90) ? ry : (W - 1 - ry);
        }
        for (int x = 0; x < cw; x++) {
            const int rx = x * H / cw;
            const int sy = (rot == IMU_ROT_90) ? (H - 1 - rx) : rx;
            map_y[x] = sy * W;
        }
        for (int y = 0; y < ch; y++) {
            const uint16_t *srow = src + map_x[y];
            uint16_t *drow = dst + y * cw;
            for (int x = 0; x < cw; x++) {
                drow[x] = srow[map_y[x]];
            }
        }
        return;
    }

    // 横握（0/180）：画布 320x240，直接缩放；180 时源坐标反向。
    for (int x = 0; x < cw; x++) {
        map_x[x] = (rot == IMU_ROT_0) ? (x * W / cw) : (W - 1 - x * (W - 1) / (cw - 1));
    }
    for (int y = 0; y < ch; y++) {
        const int sy = (rot == IMU_ROT_0) ? (y * H / ch) : (H - 1 - y * (H - 1) / (ch - 1));
        map_y[y] = sy * W;
    }
    for (int y = 0; y < ch; y++) {
        const uint16_t *srow = src + map_y[y];
        uint16_t *drow = dst + y * cw;
        for (int x = 0; x < cw; x++) {
            drow[x] = srow[map_x[x]];
        }
    }
}

// 顶部常驻状态行：IP + 张数 / 配网提示。只画在画布上，不进 JPEG。
// 竖握画布只有 240 宽，放不下就退到 scale=1，保证整行都在画面内。
static void canvas_draw_status(uint16_t *canvas, int cw, int ch)
{
    char status[48];
    if (wifi_web_connected()) {
        snprintf(status, sizeof(status), "%s #%d", wifi_web_ip(), photo_store_count());
    } else if (wifi_web_in_setup()) {
        snprintf(status, sizeof(status), "SETUP: LennonCam-Setup");
    } else {
        snprintf(status, sizeof(status), "NO WIFI #%d", photo_store_count());
    }

    int scale = 2;
    if (ui_text_width(status, scale) > cw - 8) {
        scale = 1;
    }
    const int band = UI_TEXT_H(scale) + 4;
    for (int i = 0; i < band * cw; i++) {
        canvas[i] = 0x0000;     // 状态条刷黑
    }
    ui_text_draw(canvas, cw, ch, (cw - ui_text_width(status, scale)) / 2, 2, status,
                 bsp_lcd_color(UI_COLOR_WHITE), scale);
}

// 画布 -> LCD 逻辑帧。逻辑帧固定跟着板子走：用户视角 = R(-rot)·逻辑帧，
// 所以逻辑帧 = R(rot)·画布。rot=0 直接送；其余按顺时针 90/180/270 转。
// 90/270 的方向必须随 rot 取反（两个竖握相差 180°，固定一个方向会有一个是倒的）；
// 若实拍发现竖握整体反了 180°，只需交换 90 与 270 两支的索引。
static void lcd_show_canvas(const uint16_t *canvas, int cw, int ch, imu_rot_t rot)
{
    if (rot == IMU_ROT_0) {
        bsp_lcd_draw_bitmap(0, 0, cw, ch, canvas);
        return;
    }

    for (int y = 0; y < BSP_LCD_V_RES; y++) {
        uint16_t *drow = s_lcd_buf + y * BSP_LCD_H_RES;
        for (int x = 0; x < BSP_LCD_H_RES; x++) {
            if (rot == IMU_ROT_90) {            // 顺时针 90：画布 240x320 -> 逻辑 320x240
                drow[x] = canvas[(ch - 1 - x) * cw + y];
            } else if (rot == IMU_ROT_270) {    // 顺时针 270（逆时针 90）
                drow[x] = canvas[x * cw + (cw - 1 - y)];
            } else {                            // 180：画布 320x240，横竖不变
                drow[x] = canvas[(ch - 1 - y) * cw + (cw - 1 - x)];
            }
        }
    }
    bsp_lcd_draw_bitmap(0, 0, BSP_LCD_H_RES, BSP_LCD_V_RES, s_lcd_buf);
}

static uint16_t *view_buf_alloc(void)
{
    return (uint16_t *)heap_caps_malloc(BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// 预览与回看共用：按当前方向把这一帧摆正上屏。
static void lcd_show_frame(const camera_fb_t *fb, imu_rot_t rot)
{
    if (!s_lcd_ok || !fb || fb->format != PIXFORMAT_RGB565 || fb->width <= 0 || fb->height <= 0) {
        return;
    }
    if (!s_lcd_buf) {
        s_lcd_buf = view_buf_alloc();
    }
    if (!s_view_buf) {
        s_view_buf = view_buf_alloc();
    }
    if (!s_lcd_buf || !s_view_buf) {
        ESP_LOGE(TAG, "no PSRAM for preview buffers");
        return;
    }

    const bool portrait = (rot == IMU_ROT_90 || rot == IMU_ROT_270);
    const int cw = portrait ? BSP_LCD_V_RES : BSP_LCD_H_RES;   // 240 / 320
    const int ch = portrait ? BSP_LCD_H_RES : BSP_LCD_V_RES;   // 320 / 240

    compose_view(fb, rot, s_view_buf, cw, ch);
    canvas_draw_status(s_view_buf, cw, ch);
    lcd_show_canvas(s_view_buf, cw, ch, rot);
}

static void do_capture(void)
{
    char rel_path[32] = {0};
    char full_path[48] = {0};
    camera_fb_t *fb = NULL;
    uint8_t *jpg = NULL;
    size_t jpg_len = 0;

    // 卡可能刚插上：拍照前重试一次挂载
    if (!bsp_sd_mounted()) {
        bsp_sd_mount();
    }
    if (!bsp_sd_mounted()) {
        ESP_LOGE(TAG, "PHOTO FAILED reason=no tf card");
        show_message("NO TF CARD", NULL, UI_COLOR_RED, PREVIEW_WARN_MS);
        return;
    }
    photo_store_begin();    // 保证 DCIM 存在、序号是卡上最大值 +1

    discard_frames(2);      // 丢两帧，拍按下之后的新画面（分辨率恒为 VGA，不再切换）

    camera_apply_style_preset();   // 第 3 步 CCD 色调预设挂在这里

    fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "PHOTO FAILED reason=no frame from sensor");
        return;
    }
    ESP_LOGI(TAG, "captured %dx%d fmt=%d len=%u", fb->width, fb->height,
             (int)fb->format, (unsigned)fb->len);

    // 成片保持传感器原图：不旋转、不写方向日志，直接 frame2jpg 存卡（与 v3.2 一致）。
    if (!frame2jpg(fb, JPEG_QUALITY, &jpg, &jpg_len)) {
        ESP_LOGE(TAG, "PHOTO FAILED reason=frame2jpg");
        esp_camera_fb_return(fb);
        return;
    }

    esp_err_t err = photo_store_save_jpeg(jpg, jpg_len, rel_path, sizeof(rel_path),
                                          full_path, sizeof(full_path));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "PHOTO SAVED %s size=%u", rel_path, (unsigned)jpg_len);
        bsp_audio_play_shutter();       // 存卡成功后一声“咔嚓”（不可用/失败则静音）
    } else {
        ESP_LOGE(TAG, "PHOTO FAILED reason=write %s (%s)", rel_path, esp_err_to_name(err));
        show_message("PHOTO FAIL", "WRITE ERROR", UI_COLOR_RED, PREVIEW_WARN_MS);
    }

    // 回看 3 秒：同一张未旋转的成片帧，按当前握持方向摆正显示。
    lcd_show_frame(fb, imu_orient_poll_rotation());
    vTaskDelay(pdMS_TO_TICKS(REVIEW_HOLD_MS));

    free(jpg);
    esp_camera_fb_return(fb);
}

// --------------------------------------------------------------------------
// 取景 + 拍照任务（摄像头只由这一个任务碰，避免并发取帧）
// --------------------------------------------------------------------------
static void camera_task(void *arg)
{
    bool preview_logged = false;

    while (true) {
        if (s_shoot_request) {
            s_shoot_request = false;
            do_capture();
            preview_logged = false;
            continue;
        }

        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (!preview_logged) {
            ESP_LOGI(TAG, "CAMERA PREVIEW OK (%dx%d rgb565 %u bytes)",
                     fb->width, fb->height, (unsigned)fb->len);
            preview_logged = true;
        }

        if (s_lcd_ok) {
            // 每帧读一次 IMU（带迟滞），画面随握持方向转正。
            lcd_show_frame(fb, imu_orient_poll_rotation());
        }
        preview_mailbox_service(fb);    // 网页取景请求：在 fb_return 之前编码
        esp_camera_fb_return(fb);
    }
}

// --------------------------------------------------------------------------
void app_main(void)
{
    ESP_LOGI(TAG, "camera app start, IDF %s", IDF_VER);
    ESP_LOGI(TAG, "PSRAM free: %u bytes", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // 1) I2C0
    if (bsp_i2c_init() != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed, nothing else will work");
        return;
    }

    // 1b) IMU 方向（复用刚建好的 I2C0；失败只打警告，相机照常工作）
    if (imu_orient_init() != ESP_OK) {
        ESP_LOGW(TAG, "IMU unavailable, preview stays landscape");
    }

    // 2) 扩展芯片 + 摄像头上电（bit2 = 0）
    esp_err_t err = pca9557_init();
    if (err == ESP_OK) {
        err = camera_power_on();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera power-on failed: %s", esp_err_to_name(err));
    }

    // 2b) 快门音（I2S TX + ES8311，PA 走 PCA9557 bit1）。任一步失败只警告，相机照常。
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "audio unavailable, shutter stays silent");
    }

    // 3) 屏（无卡提示要能显示，所以屏先于卡初始化）
    s_lcd_ok = (bsp_lcd_init() == ESP_OK);
    if (!s_lcd_ok) {
        ESP_LOGE(TAG, "LCD init failed, running headless");
    }

    // 4) TF 卡：失败不崩，只在屏上提示
    bool sd_ok = (bsp_sd_mount() == ESP_OK);
    if (!sd_ok) {
        show_message("NO TF CARD", "PREVIEW ONLY", UI_COLOR_RED, PREVIEW_WARN_MS);
    } else {
        photo_store_begin();
    }

    // 5) 摄像头（VGA RGB565 采集，预览缩小上屏）
    err = bsp_camera_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CAMERA FAILED reason=init (%s)", esp_err_to_name(err));
        show_message("CAMERA FAIL", "CHECK FPC", UI_COLOR_RED, 0);
        return;
    }

    // 6) 取景 JPEG 信箱（网页实时取景用；失败不影响拍照）
    s_prev_buf = (uint8_t *)heap_caps_malloc(PREVIEW_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_prev_buf) {
        ESP_LOGW(TAG, "no PSRAM for preview buffer, /preview.jpg disabled");
    }
    s_prev_mutex = xSemaphoreCreateMutex();
    s_prev_sem = xSemaphoreCreateBinary();
    if (s_prev_sem) {
        xSemaphoreTake(s_prev_sem, 0);      // 初始化为已 take 的空状态
    }

    // 7) 按键 + 取景
    button_init();
    xTaskCreatePinnedToCore(camera_task, "camera", 8192, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(button_task, "button", 3072, NULL, 4, NULL, 0);

    ESP_LOGI(TAG, "ready: BOOT(GPIO%d) = shutter", BSP_BUTTON_SHUTTER);

    // 8) WiFi + 网页相册（失败只打日志，不影响拍照）
    if (!wifi_web_start()) {
        ESP_LOGW(TAG, "wifi/web start failed, camera still works");
    }
}
