// 快门音：I2S TX -> ES8311 DAC -> 板载功放。
//
// 引脚与参数出处（~/muse-esp32-s3-lichuang，只读参考，未改动）：
//   esp32/components/muse/boards/board_lichuang_szp.c
//     :77  I2S_MCLK  = GPIO38
//     :78  I2S_BCLK  = GPIO14
//     :79  I2S_DIN   = GPIO12（本模块只播放，不占 DIN）
//     :80  I2S_DOUT  = GPIO45
//     :81  I2S_WS    = GPIO13
//     :74  EXP_PA_EN = PCA9557 bit1（功放使能）
//     :310-330 I2S_NUM_0 master / 16-bit / 2 slots / 16 kHz（MUSE_AUDIO_RATE）
//     :334 ES8311 I2C 地址 ES8311_CODEC_DEFAULT_ADDR（0x30，8 位写法）
//     :341-363 ES8311 DAC 模式、use_mclk、PA 由 PCA9557 控制
//   采样率 16000 见 esp32/components/muse/muse_audio.h:31（MUSE_AUDIO_RATE）。

#include "bsp_audio.h"

#include <math.h>

#include "board_config.h"
#include "bsp_i2c.h"

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio";

#define AUDIO_I2S_PORT      I2S_NUM_0
#define AUDIO_MCLK_PIN      GPIO_NUM_38
#define AUDIO_BCLK_PIN      GPIO_NUM_14
#define AUDIO_WS_PIN        GPIO_NUM_13
#define AUDIO_DOUT_PIN      GPIO_NUM_45
#define AUDIO_SAMPLE_RATE   16000
#define AUDIO_CHANNELS      2
#define AUDIO_VOLUME_PCT    70      // 中等音量（约七成）

// 快门音：单段 90ms 上升调“吱”（Muse 固件 muse_audio_chirp 同款）
#define CHIRP_MS            90
#define SHUTTER_FRAMES      (AUDIO_SAMPLE_RATE * CHIRP_MS / 1000)

static i2s_chan_handle_t s_tx = NULL;
static esp_codec_dev_handle_t s_spk = NULL;
static bool s_ok = false;

static int16_t s_pcm[SHUTTER_FRAMES * AUDIO_CHANNELS];


// 快门音 = Muse 固件的“吱”（muse_audio_chirp 上升调，原样移植）：
// 90ms 内频率 620->1180Hz 扫频，正弦 + 0.3 倍三次谐波（一点 8-bit 味），
// 包络 10% 淡入 + 二次衰减，峰值 7000 不削波；双声道同值。
static void synth_shutter(void)
{
    float phase = 0.0f;
    for (int i = 0; i < SHUTTER_FRAMES; i++) {
        const float x = (float)i / (float)SHUTTER_FRAMES;
        const float f = 620.0f + (1180.0f - 620.0f) * x;
        phase += 2.0f * (float)M_PI * f / (float)AUDIO_SAMPLE_RATE;
        const float env = (x < 0.1f ? x / 0.1f : 1.0f) * (1.0f - x) * (1.0f - x);
        const float s = sinf(phase) + 0.3f * sinf(3.0f * phase);
        const int16_t v = (int16_t)(s * env * 7000.0f);
        s_pcm[2 * i] = s_pcm[2 * i + 1] = v;
    }
}

// 失败收尾：放掉已拿到的资源、置不可用。调用方只打警告，不影响相机。
static void audio_disable(void)
{
    s_ok = false;
    if (s_spk) {
        esp_codec_dev_close(s_spk);
        esp_codec_dev_delete(s_spk);
        s_spk = NULL;
    }
    if (s_tx) {
        i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    // 功放保持关闭（PCA9557 bit1=0），避免无声时白白耗电
    pca9557_set_bit(PCA9557_BIT_PA_EN, 0);
}

bool bsp_audio_available(void)
{
    return s_ok;
}

esp_err_t bsp_audio_init(void)
{
    if (s_ok) {
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_master_get_bus_handle(BSP_I2C_PORT, &bus);
    if (err != ESP_OK || !bus) {
        ESP_LOGW(TAG, "no I2C%d bus (%s), audio disabled", BSP_I2C_PORT, esp_err_to_name(err));
        return (err != ESP_OK) ? err : ESP_ERR_INVALID_STATE;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;     // 播完自动补静音，功放不会一直响最后一帧
    err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s_new_channel failed (%s), audio disabled", esp_err_to_name(err));
        audio_disable();
        return err;
    }

    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AUDIO_MCLK_PIN,
            .bclk = AUDIO_BCLK_PIN,
            .ws = AUDIO_WS_PIN,
            .dout = AUDIO_DOUT_PIN,
            .din = I2S_GPIO_UNUSED,
        },
    };
    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_tx);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s tx init failed (%s), audio disabled", esp_err_to_name(err));
        audio_disable();
        return err;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = AUDIO_I2S_PORT,
        .tx_handle = s_tx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (!data_if || !ctrl_if || !gpio_if) {
        ESP_LOGW(TAG, "codec interfaces failed, audio disabled");
        audio_disable();
        return ESP_ERR_NO_MEM;
    }

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = GPIO_NUM_NC,      // 功放由 PCA9557 bit1 控制，不走 codec GPIO
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec = es8311_codec_new(&es_cfg);
    if (!codec) {
        ESP_LOGW(TAG, "ES8311 not responding, audio disabled");
        audio_disable();
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t out_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec,
        .data_if = data_if,
    };
    s_spk = esp_codec_dev_new(&out_cfg);
    if (!s_spk) {
        ESP_LOGW(TAG, "esp_codec_dev_new failed, audio disabled");
        audio_disable();
        return ESP_ERR_NO_MEM;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = AUDIO_SAMPLE_RATE,
        .channel = AUDIO_CHANNELS,
        .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "codec open failed, audio disabled");
        audio_disable();
        return ESP_FAIL;
    }
    esp_codec_dev_set_out_vol(s_spk, AUDIO_VOLUME_PCT);

    // 播放通道就绪后再开功放，避免上电噪声
    err = pca9557_set_bit(PCA9557_BIT_PA_EN, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PA enable failed (%s), audio disabled", esp_err_to_name(err));
        audio_disable();
        return err;
    }

    s_ok = true;
    ESP_LOGI(TAG, "ES8311 ready: %d Hz 16-bit %dch vol %d%% (MCLK=%d BCLK=%d WS=%d DOUT=%d)",
             AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_VOLUME_PCT,
             AUDIO_MCLK_PIN, AUDIO_BCLK_PIN, AUDIO_WS_PIN, AUDIO_DOUT_PIN);
    return ESP_OK;
}

void bsp_audio_play_shutter(void)
{
    if (!s_ok || !s_spk) {
        return;
    }
    synth_shutter();
    // 16kHz / 16bit / 2ch：90ms 的缓冲，一次写完；DMA 播完由 auto_clear 补静音。
    if (esp_codec_dev_write(s_spk, s_pcm, sizeof(s_pcm)) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "shutter playback failed");
    }
}
