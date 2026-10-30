#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define PHOTO_DIR        "/sdcard/DCIM"
#define PHOTO_DIR_REL    "/DCIM"
#define PHOTO_NAME_LEN   12     // "IMG_0001.jpg"

// 建目录 + 扫卡上已有的 IMG_xxxx.jpg，把计数器设成最大值 +1（重启不覆盖），
// 同时统计卡上 IMG 文件总数。
// 没挂卡就返回错误，计数器保持 1、张数为 0。
esp_err_t photo_store_begin(void);

// 保存一帧 JPEG。成功后：
//   rel_path  = "/DCIM/IMG_0001.jpg"（日志用）
//   full_path = "/sdcard/DCIM/IMG_0001.jpg"（fopen 用）
esp_err_t photo_store_save_jpeg(const uint8_t *data, size_t len,
                                char *rel_path, size_t rel_len,
                                char *full_path, size_t full_len);

// 卡上 IMG 照片张数（scan 时统计，save 成功 +1，delete 成功 -1）。
int photo_store_count(void);

// 收集 DCIM 里全部 IMG 序号，按降序写入 idx_out（最多 max_entries 个）。
// 返回实际找到的总数，可能大于 max_entries。没有卡/没有照片返回 0。
int photo_store_list(uint16_t *idx_out, int max_entries);

// 严格校验文件名：必须是 IMG_%04d.jpg 且恰好 12 字符。
// 删除与 /img/ 下载共用这一条，杜绝 ../ 之类的路径穿越。
bool photo_store_name_valid(const char *name);

// 删除 DCIM 下的一张照片。name 非法返回 ESP_ERR_INVALID_ARG（绝不拼路径）。
esp_err_t photo_store_delete(const char *name);
