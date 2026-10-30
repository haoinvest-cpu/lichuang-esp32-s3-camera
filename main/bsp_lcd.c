#include "bsp_lcd.h"

#include <string.h>

#include "board_config.h"
#include "bsp_i2c.h"
#include "ui_text.h"

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lcd";

static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;
static uint16_t *s_msg_buf = NULL;      // 320x240x2，只在提示/回看时用

#define LCD_BL_LEDC_CH   LEDC_CHANNEL_0
#define LCD_BL_LEDC_TMR  LEDC_TIMER_0

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t tmr = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LCD_BL_LEDC_TMR,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t ch = {
        .gpio_num = BSP_LCD_PIN_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_BL_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LCD_BL_LEDC_TMR,
        .duty = 0,
        .hpoint = 0,
        // 背光低电平点亮：SZP_C:262-271
        .flags.output_invert = true,
    };
    esp_err_t err = ledc_timer_config(&tmr);
    if (err != ESP_OK) {
        return err;
    }
    return ledc_channel_config(&ch);
}

esp_err_t bsp_lcd_backlight_set(int percent)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_BL_LEDC_CH, (1023 * percent) / 100);
    if (err == ESP_OK) {
        err = ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_BL_LEDC_CH);
    }
    return err;
}

esp_err_t bsp_lcd_init(void)
{
    esp_err_t err = backlight_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backlight init failed: %s", esp_err_to_name(err));
        return err;
    }

    const spi_bus_config_t buscfg = {
        .sclk_io_num = BSP_LCD_PIN_CLK,
        .mosi_io_num = BSP_LCD_PIN_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        // 一次 draw_bitmap 可能就送整屏，缓冲上限必须够：SZP_C:334
        .max_transfer_sz = BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t),
    };
    err = spi_bus_initialize(BSP_LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BSP_LCD_PIN_CS,      // NC，片选在 PCA9557 bit0
        .dc_gpio_num = BSP_LCD_PIN_DC,
        .spi_mode = BSP_LCD_SPI_MODE,       // SZP_C:345
        .pclk_hz = BSP_LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_HOST, &io_cfg, &s_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(err));
        return err;
    }

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BSP_LCD_PIN_RST,  // NC -> 驱动发 SWRESET
        // 官方例程用 RGB 元素序（SZP_C:353），摄像头帧直接送屏。若实机预览红蓝反了，
        // 先试把这里改成 LCD_RGB_ELEMENT_ORDER_BGR —— 这是 GC0308 RGB565 直出到
        // ST7789 的常见现象，未上机核实。
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7789 failed: %s", esp_err_to_name(err));
        return err;
    }

    // CS 仍为高时先发一次 SWRESET，把 SPI mode 2 的空闲电平喂进去
    // （MUSE_MD:27-30、MUSE_C:237-241）。此时面板忽略命令，只为时钟相位。
    esp_lcd_panel_reset(s_panel);
    // 选中 LCD 后真正复位一次。
    pca9557_set_bit(PCA9557_BIT_LCD_CS, 0);
    esp_lcd_panel_reset(s_panel);
    // ST7789 sleep-in 下 SWRESET 后要 >=120ms 才能收 SLPOUT，
    // IDF 驱动只等 20ms（st7789.c:174-178，MUSE_MD:30）。
    vTaskDelay(pdMS_TO_TICKS(120));

    err = esp_lcd_panel_init(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed: %s", esp_err_to_name(err));
        return err;
    }
    esp_lcd_panel_invert_color(s_panel, true);       // SZP_C:361
    esp_lcd_panel_swap_xy(s_panel, true);            // SZP_C:362
    esp_lcd_panel_mirror(s_panel, true, false);      // SZP_C:363
    esp_lcd_panel_disp_on_off(s_panel, true);
    bsp_lcd_backlight_set(100);

    ESP_LOGI(TAG, "ST7789 %dx%d ready (SPI3 mode%d %dMHz, CS=PCA9557 bit0)",
             BSP_LCD_H_RES, BSP_LCD_V_RES, BSP_LCD_SPI_MODE,
             (int)(BSP_LCD_PIXEL_CLOCK_HZ / 1000000));
    return ESP_OK;
}

esp_err_t bsp_lcd_draw_bitmap(int x_start, int y_start, int x_end, int y_end, const void *data)
{
    if (!s_panel || !data) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_lcd_panel_draw_bitmap(s_panel, x_start, y_start, x_end, y_end, data);
}

static uint16_t *msg_buf_get(void)
{
    if (!s_msg_buf) {
        s_msg_buf = (uint16_t *)heap_caps_malloc(
            BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return s_msg_buf;
}

esp_err_t bsp_lcd_fill(uint16_t rgb565)
{
    uint16_t *buf = msg_buf_get();
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    uint16_t px = bsp_lcd_color(rgb565);
    for (int i = 0; i < BSP_LCD_H_RES * BSP_LCD_V_RES; i++) {
        buf[i] = px;
    }
    return bsp_lcd_draw_bitmap(0, 0, BSP_LCD_H_RES, BSP_LCD_V_RES, buf);
}

esp_err_t bsp_lcd_message(const char *line1, const char *line2, uint16_t rgb565)
{
    uint16_t *buf = msg_buf_get();
    if (!buf) {
        ESP_LOGE(TAG, "no PSRAM for message buffer");
        return ESP_ERR_NO_MEM;
    }
    uint16_t fg = bsp_lcd_color(rgb565);
    uint16_t bg = bsp_lcd_color(UI_COLOR_BLACK);
    for (int i = 0; i < BSP_LCD_H_RES * BSP_LCD_V_RES; i++) {
        buf[i] = bg;
    }

    const int scale = 3;
    int y = BSP_LCD_V_RES / 2 - 20;
    int w1 = ui_text_width(line1, scale);
    ui_text_draw(buf, BSP_LCD_H_RES, BSP_LCD_V_RES, (BSP_LCD_H_RES - w1) / 2, y, line1, fg, scale);
    if (line2 && line2[0]) {
        int w2 = ui_text_width(line2, scale);
        ui_text_draw(buf, BSP_LCD_H_RES, BSP_LCD_V_RES,
                     (BSP_LCD_H_RES - w2) / 2, y + 7 * scale + 10, line2, fg, scale);
    }
    ESP_LOGI(TAG, "show message: %s%s%s", line1, line2 ? " / " : "", line2 ? line2 : "");
    return bsp_lcd_draw_bitmap(0, 0, BSP_LCD_H_RES, BSP_LCD_V_RES, buf);
}
