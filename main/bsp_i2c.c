#include "bsp_i2c.h"
#include "board_config.h"

#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_i2c";

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_pca = NULL;

esp_err_t bsp_i2c_init(void)
{
    if (s_bus) {
        return ESP_OK;
    }
    const i2c_master_bus_config_t cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = BSP_I2C_PORT,
        .scl_io_num = BSP_I2C_SCL_PIN,
        .sda_io_num = BSP_I2C_SDA_PIN,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "I2C%d ready (SDA=%d SCL=%d %dHz)", BSP_I2C_PORT,
             BSP_I2C_SDA_PIN, BSP_I2C_SCL_PIN, BSP_I2C_FREQ_HZ);
    return ESP_OK;
}

static esp_err_t pca_add_device(void)
{
    if (s_pca) {
        return ESP_OK;
    }
    if (!s_bus) {
        return ESP_ERR_INVALID_STATE;
    }
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCA9557_ADDR,
        .scl_speed_hz = BSP_I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(s_bus, &dev_cfg, &s_pca);
}

static esp_err_t pca_write_reg(uint8_t reg, uint8_t val)
{
    esp_err_t err = pca_add_device();
    if (err != ESP_OK) {
        return err;
    }
    const uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_pca, buf, sizeof(buf), 1000);
}

static esp_err_t pca_read_reg(uint8_t reg, uint8_t *val)
{
    esp_err_t err = pca_add_device();
    if (err != ESP_OK) {
        return err;
    }
    return i2c_master_transmit_receive(s_pca, &reg, 1, val, 1, 1000);
}

esp_err_t pca9557_init(void)
{
    esp_err_t err = pca_write_reg(PCA9557_REG_OUTPUT, PCA9557_OUTPUT_DEFAULT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCA9557 output write failed: %s", esp_err_to_name(err));
        return err;
    }
    err = pca_write_reg(PCA9557_REG_CONFIG, PCA9557_CONFIG_DEFAULT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCA9557 config write failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "PCA9557 @0x%02x ready (out=0x%02x cfg=0x%02x)",
             PCA9557_ADDR, PCA9557_OUTPUT_DEFAULT, PCA9557_CONFIG_DEFAULT);
    return ESP_OK;
}

esp_err_t pca9557_set_bit(uint8_t bit, int level)
{
    uint8_t cur = 0;
    esp_err_t err = pca_read_reg(PCA9557_REG_OUTPUT, &cur);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t next = level ? (cur | (1u << bit)) : (cur & ~(1u << bit));
    if (next == cur) {
        return ESP_OK;
    }
    return pca_write_reg(PCA9557_REG_OUTPUT, next);
}

esp_err_t camera_power_on(void)
{
    esp_err_t err = pca9557_set_bit(PCA9557_BIT_DVP_PWDN, 0);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "camera power on (PCA9557 bit%d = 0)", PCA9557_BIT_DVP_PWDN);
        // 让摄像头电源稳定后再初始化 SCCB；官方例程是紧接着调用 esp_camera_init
        // （SZP_C:445），这里多给 10ms 无副作用。
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return err;
}
