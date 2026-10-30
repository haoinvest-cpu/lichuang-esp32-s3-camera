#include "bsp_sd.h"

#include <stdio.h>

#include "board_config.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

static const char *TAG = "sd";

static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;

bool bsp_sd_mounted(void)
{
    return s_mounted;
}

esp_err_t bsp_sd_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    // v6.0 里正名是 esp_vfs_fat_mount_config_t，旧名只是 typedef（FACTS §7.2）。
    const esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,   // 官方例程写 true（SD_C:60），这里故意改掉
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;                              // 1 位 SD 模式，SD_C:72
    slot.clk = BSP_SD_PIN_CLK;                   // 47，SD_C:7
    slot.cmd = BSP_SD_PIN_CMD;                   // 48，SD_C:8
    slot.d0 = BSP_SD_PIN_D0;                     // 21，SD_C:9
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;   // SD_C:76

    esp_err_t err = esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount %s failed: %s (no card, or not FAT32)", BSP_SD_MOUNT_POINT,
                 esp_err_to_name(err));
        return err;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "mounted %s", BSP_SD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}
